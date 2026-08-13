# HAMi-core initialization under concurrency — problem depth and solution space

Independent analysis for LFX Mentorship 2026 Term 3 — *Reduce HAMi-core Initialization Lock Contention*
Reference issue: [HAMi#1662](https://github.com/Project-HAMi/HAMi/issues/1662)

---

## 0. How I worked, and what this document is not

**This document does not propose a solution.** It is not my place, or anyone's outside the maintainer group, to declare which approach this project should take. What an outside contributor *can* usefully do before that conversation is: establish how deep the problem actually goes, identify every mechanism contributing to it, and lay out the full space of known approaches with their trade-offs. Choosing among them is a discussion to have with maintainers, informed by constraints I cannot see from outside.

I also did not spend effort validating that the problem exists. It was filed against a production cluster and is tracked by the maintainers; its existence is not in question. I validated only to find **underlying causes** — which mechanisms produce it, and whether the recorded diagnosis still matches the code.

**Sources of truth, and only these two:**

1. **The repository**, at commit `5496322` (`Merge pull request #248`, 2026-08-03), which was `main` for this work. Plus `Project-HAMi/HAMi` for the device plugin.
2. **Maintainer statements.** HAMi-core's `OWNERS` lists `archlitchi` and `chaunceyjiang` as reviewers and **`archlitchi` as the sole approver**. Where a design decision is quoted below, it is from an approver or reviewer.

Everything else, including the analysis in the issue thread, I treated as hypothesis to check. Some of it did not survive checking. Where I could not test something, I say so.

All measurements run in Docker on a laptop with **no GPU**, because the paths measured are POSIX, not CUDA. Reproducible with `docker` alone.

**Harness, raw logs and full output:** https://gist.github.com/blackdragoon26/bbb735bd9b36f63176dea7da0c7c6093

---

## 1. Depth of the problem: what is actually true today

### 1.1 The recorded diagnosis is stale

The issue and the project description both name the `unified_lock` at `/tmp/vgpulock/lock`. In the tree:

```
$ grep -rn "try_lock_unified_lock\|try_unlock_unified_lock" --include="*.c" --include="*.h" .
src/utils.c:22:int try_lock_unified_lock() {
src/utils.c:40:int try_unlock_unified_lock() {
src/include/utils.h:8:int try_lock_unified_lock();
src/include/utils.h:9:int try_unlock_unified_lock();
```

Definitions and declarations only. **Zero call sites.** The `"unified_lock locked, waiting 1 second..."` string does not exist in the tree.

| Commit | Date | Author | Change |
|---|---|---|---|
| `a8b3d73` | 2024-02-22 | limengxuan (archlitchi) | `try_lock_unified_lock()` introduced around `set_task_pid()` |
| `24a0f49` | 2026-02-12 | maverick | Kept lock, added failure handling |
| `816630a` | 2026-02-26 | maverick123123 | Spin-and-sleep replaced with blocking `flock(LOCK_EX)` |
| `7970f7f` | — | Nishit Shah | `try_lock_unified_lock()` replaced by `lock_postinit()` in `postInit()` |

The device plugin still creates and bind-mounts `/tmp/vgpulock` into every vGPU container (`server.go:680-690`), so the lock file still appears to churn during pod bursts even though HAMi-core no longer uses it. **Anyone reproducing the symptom by watching that file will reach a wrong conclusion.** This is the single most important thing to know before touching this issue.

### 1.2 The initialization path as it runs today

```
cuInit()                                    libvgpu.c:932
 └─ preInit() → ensure_initialized()        libvgpu.c:871
      ├─ try_create_shrreg()                mml.c:1155   ← lockf, whole file
      └─ init_proc_slot_withlock()          mml.c:1044   ← region semaphore + O(N) procfs
 └─ real driver cuInit
 └─ postInit()                              libvgpu.c:893
      ├─ lock_postinit()                    mml.c:640    ← record lock, per-container file
      ├─ set_task_pid()                     utils.c:98   ← NVML diff + CUDA context probe
      └─ unlock_postinit()
```

### 1.3 Measured behaviour of the shared-region path

`ensure_initialized()` is NVML- and CUDA-free. I did not assume this — I linked HAMi-core's real `multiprocess_memory_limit.o` against stubs where every NVML symbol calls `abort()` with a diagnostic. No abort ever fired at any concurrency level.

N workers fork, block on a pipe, release simultaneously; each times one `ensure_initialized()`. Cache deleted between rounds. Counted with `strace -ff -e trace=openat,fcntl`:

| N | `/proc/<pid>/stat` opens | N(N+1)/2 | `fcntl` (lockf) |
|---:|---:|---:|---:|
| 1 | 1 | 1 | 2 |
| 8 | 36 | 36 | 16 |
| 32 | 528 | 528 | 64 |
| 64 | 2,080 | 2,080 | 128 |
| 128 | 8,256 | 8,256 | 256 |

**Exactly N(N+1)/2** at every point measured. `fcntl` exactly 2N. At the issue's density of 300 concurrent processes that projects to 45,150 procfs open/read/close cycles serialised behind one semaphore.

Timing (5 repeats, median, all workers successful, 8 CPUs):

| N | 1 | 8 | 32 | 64 | 128 | 256 | 512 |
|---|---|---|---|---|---|---|---|
| wall p50 (ms) | 0.69 | 2.10 | 12.02 | 17.47 | 33.78 | 96.41 | 293.51 |
| init p50 (ms) | 0.55 | 0.97 | 5.38 | 5.21 | 13.77 | 30.48 | 87.06 |

Per-process cost should stay flat if init were concurrent. It goes 0.55 → 87 ms.

**Trust the syscall counts more than the timings.** Above N=8 the box is oversubscribed, so some growth is run-queue delay. Syscall counts are a property of the algorithm and independent of machine, scheduler and CPU count.

**A trap worth recording:** my first run produced `2N-1` — linear, which would have refuted the whole quadratic hypothesis. It was wrong. HAMi-core registers `atexit(exit_handler)` (`:1160`) which frees a process's slot on exit, and my workers exited immediately after registering, so the table drained as fast as it filled. Only after holding workers resident until all N registered did the exact N(N+1)/2 appear. Anyone benchmarking this path who lets workers exit early will measure nothing and conclude there is no problem.

---

## 2. The villains

Ten mechanisms contribute. Not all are equal, and two of them are not performance problems at all.

| # | Villain | Location | Class | Evidence | Owned? |
|---|---|---|---|---|---|
| V1 | Liveness sweep on the join path | `mml.c:1101` → `:1003` | Perf, O(N²) | Measured exact | [#253](https://github.com/Project-HAMi/HAMi-core/pull/253) |
| V2 | Whole-file `lockf` across region setup | `mml.c:1266-1327` | Perf | Measured residual | **Unowned** |
| V3 | CUDA context probe per process | `utils.c:133,171` | Perf, dominant | Not measured (no GPU) | [#251](https://github.com/Project-HAMi/HAMi-core/pull/251) |
| V4 | Probe covers a single device | `utils.c:128,148` | Perf, scaling | Code-verified | **Unowned** |
| V5 | Probe exclusion narrowed node→container | `mml.c:640` + `server.go:665` | **Correctness** | Code-verified | **Unowned** |
| V6 | `getextrapid()` has no caller identity | `utils.c:73-96` | **Correctness** | Demonstrated | **Unowned** |
| V7 | One region-wide semaphore for all ops | `mml.c:847` | Perf, granularity | Code-verified | **Unowned** |
| V8 | 10s × 30 retry = 300s worst-case wait | `mml.c:26,34,854` | Availability | Code-verified | partly [#248](https://github.com/Project-HAMi/HAMi-core/pull/248) |
| V9 | Global `context_size` couples probe to accounting | `utils.c:166`, `context.c:14-24` | Correctness risk | Code-verified | touched by #251 |
| V10 | Failed detection silently disables rate limiting | `libvgpu.c:912` → watcher `:284` | **Safety** | Code-verified | **Unowned** |

### V1 — Liveness sweep on the join path

`init_proc_slot_withlock()` takes `lock_shrreg()` then calls `clear_proc_slot_nolock(1)`, which walks every occupied slot calling `proc_alive(pid)` — `fopen("/proc/<pid>/stat")` + `fscanf` + `fclose` per slot (`process_utils.h:17`) — while holding the lock every other joiner needs. The `cleaned_dead < 10` guard caps *removals*, not *checks*: by `&&` short-circuit it only stops calling `proc_alive` once ten dead slots have been found, which does not happen when slots are alive.

This is the N(N+1)/2. It is the clearest villain and the only one with a merged-ready fix in flight.

### V2 — Whole-file advisory lock across region setup

`lockf(fd, F_LOCK, SHARED_REGION_SIZE_MAGIC)` at `:1266` held to `F_ULOCK` at `:1327`. Every first-touch queues here. Inside, the already-initialised branch (the common case for processes 2..N) re-derives memory and SM limits from the environment and compares them for consistency — cheap work, but under an exclusive whole-file lock. The `initialized_flag` check at `:1271` happens *inside* the lock; a read-side check before acquiring would let the common case skip it entirely.

**Measured:** after applying #253, per-process p50 still grows 0.70 → 3.37 ms from N=32 to N=256. That residual is V2.

### V3 — CUDA context probe per process

`set_task_pid()` builds a full primary context (`cuDevicePrimaryCtxRetain`, `:133`) and tears it down (`:171`) purely so the process appears in NVML's list. Upstream measurements put this at ~31 ms on a consumer GPU and ~297 ms on A100, dominating the critical section. **I could not measure this** — no GPU.

### V4 — The probe covers a single device

Both device loops `break` after the first mapped device (`utils.c:128`, `:148`). On an 8-GPU node every process still probes one device. The approver endorsed lock sharding in July (see §4); the tree does not have it.

### V5 — Probe exclusion narrowed from node-wide to per-container

`set_task_pid()` identifies its host PID **by inference**: snapshot NVML's compute-process list, create a context so you appear, snapshot again, take the set difference. NVML's list is **node-wide** — it shows every process on the device regardless of container. The inference is sound only if nothing else can appear in the window.

That used to be guaranteed. `try_lock_unified_lock()` locked `/tmp/vgpulock/lock`, which the device plugin bind-mounts **from the host** into every container (`server.go:689-690`) — one lock, node-wide, one prober at a time.

The replacement `lock_postinit()` takes a POSIX record lock on `region_info.fd` (`mml.c:640-660`) — the shared-region cache file. The device plugin gives every container its own:

```go
// pkg/device-plugin/nvidiadevice/nvinternal/plugin/server.go:665
response.Envs["CUDA_DEVICE_MEMORY_SHARED_CACHE"] =
    fmt.Sprintf("%s/vgpu/%v.cache", hostHookPath, uuid.New().String())
```

A fresh UUID per allocation; when unset HAMi-core synthesizes a per-container path (`mml.c:1179-1216`). **The exclusion scope for host-PID detection therefore narrowed from node-wide to per-container.** Two containers sharing a GPU can now probe concurrently — and containers sharing a GPU is the entire purpose of the product.

### V6 — The detection algorithm cannot identify its own process

`getextrapid()` returns the *first* PID present in the second snapshot and absent from the first. It takes no caller identity and validates nothing. Linking the **real** `utils.o` and calling the **real** function with snapshots representing a concurrent probe:

```
  1 exclusive probe (old node-global lock)       -> returned 4242   expected 4242    OK
  2 concurrent container, listed before me       -> returned 3131   expected 4242    *** MISATTRIBUTED ***
  3 concurrent container, listed after me        -> returned 4242   expected 4242    OK
  4 three containers racing, me listed last      -> returned 3131   expected 4242    *** MISATTRIBUTED ***

2 of 4 cases misattributed.
```

Case 3 is right only by list ordering. With N concurrent probes the caller is correct with probability ~1/N. The window spans a full context retain — tens to hundreds of milliseconds.

V5 and V6 compose: V6 is the latent weakness, V5 removed the guard that made it unreachable.

**Consequence.** `hostpid` is the key the utilisation watcher uses to attribute GPU usage to a slot — `find_proc_by_hostpid()` (`watcher.c:242`, `:251`) and the hostpid match in `set_gpu_device_sm_utilization()` (`mml.c:378-385`). A container adopting another's host PID is charged the wrong process's SM utilisation and throttles on it. Separately `context_size` is read from the matched process's `usedGpuMemory` (`utils.c:162-169`).

### V7 — One region-wide semaphore

`lock_shrreg()` guards the whole shared region with a single semaphore. Joining a slot serialises against every unrelated region operation. Slots already carry a per-slot `seqlock` field, so finer-grained protection is partly present in the data structure but not used for the join.

### V8 — Long worst-case waits

`SEM_WAIT_TIME` 10 s × `SEM_WAIT_RETRY_TIMES` 30 (`mml.c:26,34`) → `LOG_ERROR("Cannot acquire lock after 5 minutes")`. Owner-death recovery exists (`:877-899`) via `proc_alive` on the recorded owner, and #248 addressed the post-init lock specifically.

### V9 — Global `context_size` couples the probe to memory accounting

`set_task_pid()` sets a single global `context_size` from the probe context's NVML footprint (`utils.c:166`), later charged against tracked usage on the app's first real `cuDevicePrimaryCtxRetain` (`cuda/context.c:14-24`). It defaults to 0 (`mml.c:54`). **Any change that removes the probe must supply this value from somewhere else**, or every process undercounts its GPU usage by a real context — a vGPU *enforcement* gap, not just bookkeeping.

### V10 — Failed detection silently disables rate limiting

If `lock_postinit()` fails or `set_task_pid()` errors, `postInit()` sets `pidfound = 0` (`libvgpu.c:912`). The utilisation watcher then loops `if (pidfound==0) { update_host_pid(); if (pidfound==0) continue; }` (`watcher.c:284-287`) and never reaches `init_gpu_device_utilization()`, so `cached_util_switch` never updates; `rate_limiter()` fast-exits when it is 0 (`watcher.c:50-52`). **A process whose host PID was never resolved runs with SM rate limiting silently off.** Failure of a performance path degrades an enforcement guarantee without erroring.

---

## 3. The solution space

Grouped by the class of problem, not by villain, because several villains share a class. **These are options, not recommendations.** Each has costs; several are mutually exclusive; some are ruled out by constraints in §4.

### 3.1 Expensive maintenance on a hot path (V1)

The general problem: periodic reclamation work attached to an operation that happens N times.

| Approach | How it applies here | Trade-off |
|---|---|---|
| **Threshold-triggered sweep** | Sweep only when occupancy crosses a watermark | What #253 does. Simple; leaves dead slots resident longer |
| **Amortised / every-k-operations** | Sweep on every k-th join | Bounds cost per join; needs a counter in shared memory |
| **Background reclamation** | A watcher thread sweeps off the join path | Removes it from init entirely; adds a thread and its own liveness question |
| **Lazy tombstoning** | Exiting processes mark slots; reclaim only when allocating and full | Partly present already — `exit_handler` sets PID 0 and PID-0 removal reads no files |
| **Epoch-based reclamation (EBR)** | Readers publish an epoch; reclaim only fully-passed epochs | Standard in lock-free structures; heavier than needed here |
| **RCU-style deferred free** | Grace-period reclamation | Kernel-standard; awkward across processes in shared memory |
| **Singleflight / coalescing** | One joiner sweeps, others reuse the result within a window | Directly targets the thundering-herd shape |
| **Cheaper liveness probe** | `kill(pid,0)` instead of `fopen`+`fscanf`+`fclose` | One syscall instead of three; still O(N), doesn't change the class |
| **pidfd** | Hold `pidfd` handles, poll for exit | Robust against PID reuse; kernel ≥5.3, more state per slot |

### 3.2 Coarse-grained locking (V2, V4, V7)

| Approach | How it applies here | Trade-off |
|---|---|---|
| **Double-checked locking** | Read `initialized_flag` before taking the `lockf`; skip if set | Small, targeted; needs correct acquire/release ordering — already used elsewhere in this file |
| **Reader-writer lock** | Fast path takes a read lock | Fits a read-mostly region; POSIX file locks already support `F_RDLCK` |
| **Lock striping / sharding** | Per-device or per-slot-bucket locks | The approver's endorsed Solution 2 shape; needs a scheme that stays correct across devices |
| **Per-slot CAS claiming** | Claim a slot with an atomic compare-exchange, no global lock | Slots already carry `_Atomic` fields and a seqlock; largest granularity win, largest change |
| **Seqlock reads** | Lock-free readers, versioned writers | Already the pattern for slot reads; extending it to join is natural |
| **Sharded regions** | Split the region into independent shards | Big change; questionable when the region is already per-container |

### 3.3 Identity across a PID namespace boundary (V3, V5, V6)

The hard one. A process inside a namespace needs its identity outside it.

| Approach | Mechanism | Trade-off |
|---|---|---|
| **Current: NVML set-difference** | Create a context, diff process lists | Self-contained; costs a full context; unsound without node-wide exclusion (V5/V6) |
| **`NStgid` in `/proc/self/status`** | Kernel exposes PID in every namespace it belongs to | Single local read, no CUDA. Only returns host PID when reading a procfs mounted in the initial PID namespace — needs a deployment contract, and a host `/proc` mount exposes other tenants |
| **`SO_PEERCRED` over a unix socket** | Kernel translates the peer's PID into the receiver's namespace | Accurate and cheap; requires a host-side listener — collides with the self-containment constraint unless an existing daemon owns it |
| **`SCM_CREDENTIALS`** | Same idea, ancillary data on a datagram | Same constraint |
| **cgroup correlation** | Match `/proc/self/cgroup` against host-side data | No new socket; still needs a host-side reader |
| **`pidfd_open` / `pidfd_getfd`** | Kernel handles instead of numeric PIDs | Namespace-stable, immune to reuse; kernel ≥5.3, doesn't itself cross the boundary |
| **Self-identifying probe (watermark)** | Make the probe distinguish *itself* rather than infer by elimination — e.g. correlate on a value only the caller knows | Would make V6 unreachable without needing exclusion at all, and stays self-contained. Still pays the context cost unless combined with something else |
| **Deferred probe** | Do detection on first real CUDA work instead of at init | Moves cost off the startup burst; changes when accounting becomes valid |
| **Amortised context** | Keep the primary context instead of releasing | Removes teardown and makes the app's own retain cheap; costs ~72 MiB+ resident per process — significant under a vGPU memory cap |
| **Per-container single probe** | One process resolves, others reuse | Does not work: PIDs are not offset-mapped, so one pair tells you nothing about another. Recorded because it looks plausible |

### 3.4 Failure-mode hardening (V8, V9, V10)

| Approach | Applies to | Note |
|---|---|---|
| **Fail-closed instead of fail-open** | V10 | Treat "host PID unresolved" as a reason to refuse or degrade loudly, not to disable limiting silently |
| **Explicit degraded state** | V10 | Surface a metric/log an operator can alert on |
| **Decouple accounting from the probe** | V9 | Source `context_size` independently — read once per driver+GPU, or lazily off the first real context |
| **Bounded backoff with jitter** | V8 | Already used in `postinit_file_lock`; `lock_shrreg` still uses fixed 10 s × 30 |
| **Owner-death detection via kernel** | V8 | Record locks release automatically on death — the reason `lock_postinit` moved to `fcntl` |

---

## 4. Constraints the maintainers have already set

From approver/reviewer comments, not the wider discussion:

- **No dependency on a monitor or daemon.** archlitchi, 2026-07-17: *"i think to rely on monitor is too heavy for HAMi-core right now, we can first implement the first two approach, and see how it improves."* HAMi depends on HAMi-core, not the reverse.
- **The reporter's Solution 3** (host-side PID service) was declined on those grounds, despite the reporter stating it performs best and runs in their production cluster.
- **Solutions 1 and 2 were endorsed.** Solution 1 (blocking lock) shipped as `816630a`. **Solution 2 — sharding the single global lock so containers on different GPUs do not block each other — was endorsed and is not in the tree** (V4).

This rules out, or at least heavily constrains, several rows in §3.3: anything requiring a new host-side component. It does not obviously rule out reusing a component that already exists and already runs privileged.

---

## 5. What is already in flight

| Item | Who | State | Covers |
|---|---|---|---|
| [HAMi-core#239](https://github.com/Project-HAMi/HAMi-core/pull/239) | om7057 | **Closed, unmerged** (guidelines) | Concurrent-init benchmark |
| [HAMi-core#247](https://github.com/Project-HAMi/HAMi-core/pull/247) | chidwipak | Open | Second benchmark |
| [HAMi-core#244](https://github.com/Project-HAMi/HAMi-core/pull/244) | iemAnshuman | Open | Region concurrency regression test |
| [HAMi-core#248](https://github.com/Project-HAMi/HAMi-core/pull/248) | iemAnshuman | **Merged** | Post-init lock recovery after owner death (V8, partly) |
| [HAMi-core#251](https://github.com/Project-HAMi/HAMi-core/pull/251) | iemAnshuman | Open | Host-PID broker client + lazy context accounting (V3, V9) |
| [HAMi#2244](https://github.com/Project-HAMi/HAMi/issues/2244) | iemAnshuman | Open | RFC: tiered host-PID discovery |
| [HAMi-core#252/253](https://github.com/Project-HAMi/HAMi-core/pull/253) | keshav9926 | Open | Slot-sweep measurement and fix (V1) |

Note the benchmark the discussion treats as its baseline (#239) was closed unmerged on contribution-guideline grounds, so the field's headline numbers are currently not reproducible from the tree.

**Unowned by anything above: V2, V4, V5, V6, V7, V10.**

### A/B of the leading in-flight fix (#253)

Both arms built from the same tree, alternated to cancel drift:

| N | procfs opens, baseline | with #253 | wall baseline | wall #253 | change |
|---:|---:|---:|---:|---:|---:|
| 32 | 528 | **0** | 7.97 ms | 4.19 ms | −47.4% |
| 64 | 2,080 | **0** | 21.70 ms | 5.33 ms | −75.4% |
| 128 | 8,256 | **0** | 39.42 ms | 9.88 ms | −74.9% |
| 256 | 32,896 | **0** | 101.12 ms | 16.98 ms | −83.2% |

The fix is real and large; below threshold the sweep disappears entirely. It does **not** flatten the curve — per-process p50 still grows 0.70 → 3.37 ms across that range, which is V2.

---

## 6. Open questions I would put to maintainers

1. **Is the V5/V6 misattribution reachable in practice?** Do two containers sharing one device observe each other in `nvmlDeviceGetComputeRunningProcesses`? NVML's semantics suggest yes and it is why the original lock was node-wide, but I have no GPU to confirm. If yes, was narrowing the exclusion scope in `7970f7f` intentional, with something else preventing the race that I have not found?
2. **Was V4 (single-device probe) meant to be addressed by the endorsed Solution 2**, or is device sharding a separate piece of work?
3. **Is V10's fail-open behaviour intentional?** Silently disabling rate limiting when host-PID detection fails favours availability over enforcement; that may be deliberate, but it is not documented.
4. **Where should `context_size` come from** if the probe is removed or deferred (V9)?
5. **Does "no external dependency" exclude reusing the device plugin's existing daemonset**, which already runs with `hostPID: true`, or only new components?

---

## 7. Limits of this work

- **No GPU.** Nothing here measures `cuInit`, `set_task_pid` or the context probe. V3's cost figures are other people's measurements, cited not reproduced.
- **V5's practical impact is unconfirmed on hardware.** The code path is verified and the algorithm's behaviour demonstrated; whether containers actually see each other in NVML on a shared device is open — question 1 above.
- **8 CPUs**, so timing points above N=8 are oversubscribed. Syscall counts are unaffected.
- **Containerised Linux on an arm64 Mac**, not a datacenter node. Absolute latencies are not comparable; syscall counts should be identical anywhere.
- **Single device**; multi-GPU behaviour untested.
- `stubs.c` provides `cuda_to_nvml_map` as identity; on the exercised paths it is used only for indexing.

---

## 8. Reproducing

No GPU required.

```bash
docker build --platform linux/arm64 -t hami-lab lab/
git clone https://github.com/Project-HAMi/HAMi-core.git lab/hami-core-rw
cd lab/hami-core-rw && git checkout 5496322 && cd -

docker run --rm --platform linux/arm64 -v "$PWD/lab:/work" hami-lab bash -c '
  cd /work && mkdir -p build && cd build && cmake /work/hami-core-rw >/dev/null 2>&1; cd /work
  INC="-Ihami-core-rw/src -Ihami-core-rw -I$NV_INC/cuda_runtime/include -I$NV_INC/nvml_dev/include -Ibuild/config"
  gcc -c hami-core-rw/src/multiprocess/multiprocess_memory_limit.c -o build/mml.o $INC -D_GNU_SOURCE -fPIC -O2
  gcc -c hami-core-rw/src/utils.c     -o build/utils.o     $INC -D_GNU_SOURCE -fPIC -O2
  gcc -c hami-core-rw/src/log_utils.c -o build/log_utils.o $INC -D_GNU_SOURCE -fPIC -O2
  gcc -c bench_shrreg.c -o build/bench.o -O2 -D_GNU_SOURCE
  gcc -c stubs.c -o build/stubs.o -O2 && gcc -c stubs_cuda.c -o build/stubs_cuda.o -O2
  gcc build/bench.o build/mml.o build/log_utils.o build/stubs.o -o build/bench_shrreg -lpthread -lrt
  gcc -c test_getextrapid.c -o build/tge.o $INC -D_GNU_SOURCE -O2
  gcc build/tge.o build/utils.o build/mml.o build/log_utils.o build/stubs.o build/stubs_cuda.o \
      -o build/test_getextrapid -lpthread -lrt'

docker run --rm --platform linux/arm64 -v "$PWD/lab:/work" hami-lab /work/build/test_getextrapid          # V6
docker run --rm --platform linux/arm64 --cpus=8 --cap-add=SYS_PTRACE -v "$PWD/lab:/work" \
    hami-lab bash /work/run_syscalls.sh                                                                   # V1 counts
docker run --rm --platform linux/arm64 --cpus=8 -v "$PWD/lab:/work" hami-lab bash /work/run_sweep.sh      # timings
```

| File | What it is |
|---|---|
| `Dockerfile` | Build env; real NVIDIA headers via pip, no driver |
| `bench_shrreg.c` | Fork N, barrier, time one `ensure_initialized()`, hold until all registered |
| `test_getextrapid.c` | V6 demonstration against the real `getextrapid()` |
| `stubs.c`, `stubs_cuda.c` | Link stubs; every NVML/CUDA symbol aborts if called |
| `run_sweep.sh`, `run_syscalls.sh`, `ab_test.sh` | Sweep, syscall count, A/B harnesses |
| `sweep_fixed.txt` | Raw JSON per run |
| `sweep_raw.txt` | Raw JSON from the **flawed** pre-hold harness (§1.3 trap) |
| `syscalls_raw.csv`, `ab_timing.csv` | Raw counts, A/B timings |
| `test_getextrapid_output.txt` | V6 output |
| `pr253.patch` | #253 as applied for the A/B |

---

## 9. Reading of the problem

The cheap fixes are done. `flock` instead of sleep-polling was twenty lines and shipped in February. What is left splits into two very different kinds of work.

One kind is mechanical: V1, V2, V4, V7 are all "a lock is wider than the thing it protects." They have textbook answers in §3.1 and §3.2, they can be measured without hardware, and #253 already shows the wins are real.

The other kind is a genuine design question the project has circled since February: **a process inside a PID namespace needs its identity outside that namespace, and every mechanism for learning that is either slow or requires trusting something outside the container.** The current answer — infer yourself by elimination from a node-wide list — is clever and self-contained, and its cost is a full CUDA context per process.

What I did not expect to find is that this trick has a second cost nobody has priced. Inference by elimination is only sound under exclusion; the exclusion it depended on was node-wide; it is now per-container. The performance work narrowed a lock, and the correctness of the algorithm depended on that lock's width. That class of bug does not appear in a benchmark. It appears as a container being throttled on somebody else's utilisation, months later, in a cluster nobody can debug — and V10 means the same failure can also end with limiting silently off.

Whether that is the most important thing here is not my call. It is the thing I would want answered before anyone writes a patch.
