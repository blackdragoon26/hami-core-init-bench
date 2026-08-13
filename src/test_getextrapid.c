/*
 * test_getextrapid.c — does host-PID detection stay correct when two containers
 * probe concurrently?
 *
 * Links against HAMi-core's real utils.o and calls the real getextrapid().
 * Nothing is reimplemented.
 *
 * Context: set_task_pid() (src/utils.c:98) identifies its own host PID by
 * snapshotting NVML's node-wide compute-process list, creating a CUDA primary
 * context so that its own process appears, snapshotting again, and asking
 * getextrapid() which PID is new. NVML's list is node-wide, so the snapshot
 * sees every container on the node, not just the caller's.
 *
 * Until commit 7970f7f that whole sequence was serialised by a lock at
 * /tmp/vgpulock/lock, which HAMi's device plugin bind-mounts from the host into
 * every vGPU container (server.go:689) -- i.e. node-global mutual exclusion.
 * It is now serialised by lock_postinit(), a POSIX record lock taken on
 * region_info.fd, the shared-region cache file. The device plugin gives every
 * container its own cache file with a fresh UUID (server.go:665), so that lock
 * excludes only processes within one container.
 *
 * These cases ask what getextrapid() returns when a second container's process
 * appears in the same window.
 */
#include <stdio.h>
#include <string.h>
#include "include/nvml_prefix.h"
#include <nvml.h>
#include "include/nvml_override.h"

extern int getextrapid(unsigned int prev, unsigned int current,
                       nvmlProcessInfo_t1 *pre_pids_on_device,
                       nvmlProcessInfo_t1 *pids_on_device);

static int failures = 0;

static void check(const char *name, int got, int want_mine, const char *note) {
    int ok = (got == want_mine);
    printf("  %-46s -> returned %-6d expected %-6d  %s\n",
           name, got, want_mine, ok ? "OK" : "*** MISATTRIBUTED ***");
    if (!ok) {
        failures++;
        printf("      %s\n", note);
    }
}

int main(void) {
    nvmlProcessInfo_t1 before[8], after[8];

    printf("HAMi-core getextrapid() — concurrent-container host PID detection\n");
    printf("real getextrapid() from src/utils.c, commit 5496322\n\n");

    /* Case 1: baseline. Only my process appears. This is the case the
       node-global lock used to guarantee. */
    memset(before, 0, sizeof(before));
    memset(after, 0, sizeof(after));
    before[0].pid = 1000;                       /* some unrelated resident proc */
    after[0].pid  = 1000;
    after[1].pid  = 4242;                       /* me */
    check("1 exclusive probe (old node-global lock)",
          getextrapid(1, 2, before, after), 4242,
          "baseline should always pass");

    /* Case 2: another container's process appears in the same window and
       happens to sort ahead of mine in NVML's list. */
    memset(before, 0, sizeof(before));
    memset(after, 0, sizeof(after));
    before[0].pid = 1000;
    after[0].pid  = 1000;
    after[1].pid  = 3131;                       /* other container, listed first */
    after[2].pid  = 4242;                       /* me */
    check("2 concurrent container, listed before me",
          getextrapid(1, 3, before, after), 4242,
          "getextrapid returns the FIRST pid present in `current` and absent from\n"
          "      `previous`. It has no way to tell which of the new pids is the caller,\n"
          "      so the caller adopts another container's host PID.");

    /* Case 3: same race, but mine happens to be listed first. Correct by luck. */
    memset(before, 0, sizeof(before));
    memset(after, 0, sizeof(after));
    before[0].pid = 1000;
    after[0].pid  = 1000;
    after[1].pid  = 4242;                       /* me, listed first */
    after[2].pid  = 3131;                       /* other container */
    check("3 concurrent container, listed after me",
          getextrapid(1, 3, before, after), 4242,
          "ordering-dependent");

    /* Case 4: three containers probing at once. */
    memset(before, 0, sizeof(before));
    memset(after, 0, sizeof(after));
    before[0].pid = 1000;
    after[0].pid  = 1000;
    after[1].pid  = 3131;
    after[2].pid  = 3132;
    after[3].pid  = 4242;                       /* me, listed last */
    check("4 three containers racing, me listed last",
          getextrapid(1, 4, before, after), 4242,
          "with N concurrent probes the caller has a 1/N chance of being right");

    printf("\n%d of 4 cases misattributed.\n", failures);
    printf("\nCases 2 and 4 are unreachable while host-PID detection holds a\n"
           "node-global lock, and reachable once it holds a per-container one.\n");
    return 0;
}
