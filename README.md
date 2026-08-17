# hami-core-init-bench

A GPU-free test bench for HAMi-core's concurrent initialization path, built while investigating
[Project-HAMi/HAMi#1662](https://github.com/Project-HAMi/HAMi/issues/1662) — *high latency when
hundreds of processes initialize `libvgpu.so` concurrently*.

Measured against [HAMi-core](https://github.com/Project-HAMi/HAMi-core) at commit **`5496322`**.

**No GPU or NVIDIA driver required.** `ensure_initialized()` is POSIX — shared memory, semaphores,
file locks and procfs — so the shared-region path can be exercised on any Linux box in Docker.
The benchmark links against HAMi-core's own `multiprocess_memory_limit.o` and `utils.o` and calls
the real functions; nothing is reimplemented.

Full write-up: **[`docs/analysis.md`](docs/analysis.md)**

---

## What it measures

### 1. Shared-region join cost is quadratic in concurrent starts

`init_proc_slot_withlock()` takes the region semaphore and calls `clear_proc_slot_nolock(1)`, which
reads `/proc/<pid>/stat` once per occupied slot while holding a lock every other joiner needs.

Counted with `strace -ff -e trace=openat,fcntl`:

| N | `/proc/<pid>/stat` opens | N(N+1)/2 | `fcntl` (lockf) |
|---:|---:|---:|---:|
| 1 | 1 | 1 | 2 |
| 8 | 36 | 36 | 16 |
| 32 | 528 | 528 | 64 |
| 64 | 2,080 | 2,080 | 128 |
| 128 | 8,256 | 8,256 | 256 |

Exactly **N(N+1)/2** at every point. Syscall counts are a property of the algorithm — independent of
CPU count, scheduler and machine — so they should reproduce identically anywhere.

Projected to the density in the issue: **45,150** procfs open/read/close cycles at 300 concurrent
processes, serialised behind one semaphore.

### 2. Host-PID detection under concurrent containers

`set_task_pid()` identifies its own host PID by set difference over NVML's **node-wide** process
list: snapshot, create a CUDA context so you appear, snapshot again, take what's new. That inference
is sound only while nothing else can appear in the window.

`getextrapid()` returns the *first* new PID and takes no caller identity. Calling the real function
with snapshots representing a concurrent probe:

```
1 exclusive probe (old node-global lock)   -> returned 4242  expected 4242   OK
2 concurrent container, listed before me   -> returned 3131  expected 4242   *** MISATTRIBUTED ***
3 concurrent container, listed after me    -> returned 4242  expected 4242   OK
4 three containers racing, me listed last  -> returned 3131  expected 4242   *** MISATTRIBUTED ***
```

See `docs/analysis.md` §2 for why the exclusion window changed and what consumes `hostpid`.

### 3. A/B against [HAMi-core#253](https://github.com/Project-HAMi/HAMi-core/pull/253)

Both arms built from the same tree, alternated to cancel drift:

| N | procfs opens (base) | with #253 | wall (base) | wall (#253) | change |
|---:|---:|---:|---:|---:|---:|
| 32 | 528 | **0** | 7.97 ms | 4.19 ms | −47.4% |
| 64 | 2,080 | **0** | 21.70 ms | 5.33 ms | −75.4% |
| 128 | 8,256 | **0** | 39.42 ms | 9.88 ms | −74.9% |
| 256 | 32,896 | **0** | 101.12 ms | 16.98 ms | −83.2% |

Large and real, but it does not flatten the curve — per-process p50 still grows 0.70 → 3.37 ms over
that range. The residual is the whole-file `lockf` in `try_create_shrreg()`.

### 4. Device mapping — against the *real* map functions

The benchmarks above link `src/stubs.c`, which provides `cuda_to_nvml_map()` and
`nvml_to_cuda_map()` as **identity**. That is fine for the shared-region paths, which use them only
for indexing — but it makes the device-mapping logic itself invisible.

`scripts/run_devmap.sh` instead compiles HAMi-core's own `utils.c` and
`multiprocess_utilization_watcher.c`, so `parse_cuda_visible_env()`, `nvml_to_cuda_map()` and
`cuda_to_nvml_map()` under test are the project's real implementations. The only thing a stub
supplies is the device count, modelling the one asymmetry that matters: **the CUDA driver honours
`CUDA_VISIBLE_DEVICES`; NVML does not.** Every other symbol aborts if called.

**a. `set_task_pid()` probes a device it may not own.** Snapshots are taken on the lowest NVML index
the container *considers* visible; the probe context is created on CUDA device 0. Because
`parse_cuda_visible_env()` initialises all 16 map entries to identity and overwrites only the first
`count`, an unassigned device can still resolve — so the snapshot can land on a GPU the container
was never given. 8 of 16 configurations diverge; 12 expose such phantom devices.

| `CVD` | Node | map | snapshot | probe | on `main` |
|---|---|---|---|---|---|
| `1` | 2 GPU | `[1,1]` | 1 | 1 | OK |
| `1,0` | 2 GPU | `[1,0]` | 0 | 1 | `host pid is error!` |
| `3` | 8 GPU | `[3,1,2,3,…]` | **1** | 3 | `host pid is error!` |

`CUDA_VISIBLE_DEVICES=1` succeeds on a two-GPU node while `=3` fails on an eight-GPU node — the
same single-device shape, opposite outcomes, because the phantom sits below the assigned device.

**b. The utilisation watcher samples the wrong physical GPU.**
`get_used_gpu_utilization()` converts an NVML index to a CUDA index and then passes that CUDA index
to `nvmlDeviceGetHandleByIndex()`, which takes an NVML index and translates nothing
(`nvml/hook.c:392`). `set_task_pid()` calls the same function correctly with the NVML index
(`utils.c:118`, `:139`). 5 of 37 loop iterations sample a device the container does not own —
including `CVD=1` on a two-GPU node, the most ordinary vGPU allocation there is.

**c. Two regression tests that fail on `main`.**

- `mergepid()` copies only `.pid`, never `usedGpuMemory` (`utils.c:86`), so the merged array's
  memory field is always zero — which is why `set_task_pid()` reads `context_size` from the *raw*
  array while matching on the merged one. With one device polled the indices coincide by accident.
  Driven with a PID present on two devices: `merged[2].pid=3003` but `raw[2].pid=3001`.
- `getextrapid()`'s guard `if (current-prev<=0)` uses two `unsigned int` operands, so it is
  identically `current == prev` and never tests `current < prev`. With five PIDs before, three
  after, and a peer container's process appearing while the caller's retain has not landed, it
  returns **9999** — a foreign PID, with the caller absent from the snapshot entirely.
  (See [HAMi-core#264](https://github.com/Project-HAMi/HAMi-core/issues/264) /
  [PR #265](https://github.com/Project-HAMi/HAMi-core/pull/265), which report the arithmetic; this
  case shows the consequence.)

These are derived from the real functions with a **simulated device count** — they predict hardware
behaviour, they do not observe it. No GPU was used.

---

## Running it

```bash
git clone https://github.com/Project-HAMi/HAMi-core.git hami-core-rw
(cd hami-core-rw && git checkout 5496322)

docker build --platform linux/arm64 -t hami-lab .

docker run --rm --platform linux/arm64 -v "$PWD:/work" hami-lab bash -c '
  cd /work && mkdir -p build && cd build && cmake /work/hami-core-rw >/dev/null 2>&1; cd /work
  INC="-Ihami-core-rw/src -Ihami-core-rw -I$NV_INC/cuda_runtime/include -I$NV_INC/nvml_dev/include -Ibuild/config"
  gcc -c hami-core-rw/src/multiprocess/multiprocess_memory_limit.c -o build/mml.o $INC -D_GNU_SOURCE -fPIC -O2
  gcc -c hami-core-rw/src/utils.c     -o build/utils.o     $INC -D_GNU_SOURCE -fPIC -O2
  gcc -c hami-core-rw/src/log_utils.c -o build/log_utils.o $INC -D_GNU_SOURCE -fPIC -O2
  gcc -c src/bench_shrreg.c -o build/bench.o -O2 -D_GNU_SOURCE
  gcc -c src/stubs.c -o build/stubs.o -O2 && gcc -c src/stubs_cuda.c -o build/stubs_cuda.o -O2
  gcc build/bench.o build/mml.o build/log_utils.o build/stubs.o -o build/bench_shrreg -lpthread -lrt
  gcc -c src/test_getextrapid.c -o build/tge.o $INC -D_GNU_SOURCE -O2
  gcc build/tge.o build/utils.o build/mml.o build/log_utils.o build/stubs.o build/stubs_cuda.o \
      -o build/test_getextrapid -lpthread -lrt'

# host-PID misattribution cases
docker run --rm --platform linux/arm64 -v "$PWD:/work" hami-lab /work/build/test_getextrapid

# syscall counts
docker run --rm --platform linux/arm64 --cpus=8 --cap-add=SYS_PTRACE \
  -v "$PWD:/work" hami-lab bash /work/scripts/run_syscalls.sh

# timing sweep
docker run --rm --platform linux/arm64 --cpus=8 -v "$PWD:/work" hami-lab bash /work/scripts/run_sweep.sh

# device mapping: truth table, watcher device, guard/merge regressions
docker run --rm --platform linux/arm64 -v "$PWD:/work" hami-lab bash /work/scripts/run_devmap.sh
```

Drop `--platform linux/arm64` on an x86 host.

---

## Method notes

**NVML stubs abort rather than fake.** The claim "the init path never calls NVML" is load-bearing, so
`src/stubs.c` and `src/stubs_cuda.c` make every NVML/CUDA symbol `abort()` with a diagnostic instead
of returning a plausible value. No abort has ever fired — the premise is confirmed by construction,
not by reading.

**Workers must stay resident.** HAMi-core registers `atexit(exit_handler)`, which frees a process's
slot on exit. An early version of this harness let workers exit immediately after registering; slots
drained as fast as they filled and the count came out at `2N-1` — linear, which would have refuted
the whole hypothesis. `src/bench_shrreg.c` holds every worker until all N have registered.
`results/sweep_raw.txt` is retained as the output of that flawed version.

**Trust the syscall counts over the timings.** Measurements were taken with `--cpus=8`, so every
point above N=8 is oversubscribed and part of the timing growth is run-queue delay. The syscall
counts have no such confound.

---

## Layout

| Path | Contents |
|---|---|
| `Dockerfile` | Build env; real NVIDIA headers via pip, no driver |
| `src/bench_shrreg.c` | Fork N, barrier, time one `ensure_initialized()`, hold until all registered |
| `src/test_getextrapid.c` | Concurrent-container cases against the real `getextrapid()` |
| `src/stubs.c`, `src/stubs_cuda.c` | Link stubs; every NVML/CUDA symbol aborts if called |
| `src/test_device_map.c` | CVD → device-map truth table, real `parse_cuda_visible_env()` |
| `src/test_watcher_device.c` | Which physical GPU `get_used_gpu_utilization()` samples |
| `src/test_guard_merge.c` | Regression tests: `getextrapid()` guard, `mergepid()` field loss |
| `src/stubs_devmap.c`, `src/stubs_devmap_trap.c` | Device-count oracle; everything else aborts |
| `scripts/run_sweep.sh` | Timing sweep, N=1..512 |
| `scripts/run_syscalls.sh` | strace syscall counting |
| `scripts/ab_test.sh` | Interleaved A/B harness |
| `scripts/run_devmap.sh` | Device-mapping experiments (section 4) |
| `results/` | Raw logs behind every number above |
| `patches/pr253.patch` | HAMi-core#253 as applied for the A/B |
| `docs/analysis.md` | Full write-up: problem depth, contributing mechanisms, solution space |

---

## Disclosure

The device-mapping experiments in section 4 — `src/test_device_map.c`,
`src/test_watcher_device.c`, `src/test_guard_merge.c`, `src/stubs_devmap*.c` and
`scripts/run_devmap.sh` — were written with AI assistance (Claude).

What that did and did not cover, stated plainly:

- Every finding is derived from HAMi-core's **own functions** at `5496322`, not from a
  description of them. The tests link `utils.c` and `multiprocess_utilization_watcher.c`
  directly, and every other symbol is an `abort()` trap.
- Every number in `results/` is regenerated by `scripts/run_devmap.sh` and reproducible by
  anyone with Docker. Nothing is quoted from memory.
- Prior art was checked against the HAMi-core issue tracker and all 43 comments on
  Project-HAMi/HAMi#1662 before anything here was described as new. Three findings that turned
  out to be already filed are credited in `docs/analysis.md` rather than claimed
  ([#276](https://github.com/Project-HAMi/HAMi-core/issues/276),
  [#266](https://github.com/Project-HAMi/HAMi-core/issues/266),
  [#264](https://github.com/Project-HAMi/HAMi-core/issues/264)).
- These results use a **simulated device count**. They predict hardware behaviour; they do not
  observe it. No GPU was involved.

The review, verification and conclusions are mine.

---

## Environment these results came from

| | |
|---|---|
| HAMi-core | `5496322` |
| Container | `ubuntu:22.04`, `--platform linux/arm64`, `--cpus=8` |
| Kernel | 6.12.76-linuxkit, aarch64 |
| Host | Apple silicon, macOS 25.4.0 |
| GPU | none |

Absolute latencies are not comparable to a datacenter node. The syscall counts should be.

---

Test bench only — not affiliated with or endorsed by Project-HAMi.
HAMi-core is Apache-2.0; this repository reads and links against it for measurement.
