/* X0c — regression tests against HAMi-core's real getextrapid() and mergepid().
 *
 * Claims under test:
 *   C3 — the guard `if (current-prev<=0) return 0;` (utils.c:82) uses two
 *        unsigned int operands, so the expression is identically
 *        `current == prev`.  It has never tested current < prev, which is the
 *        case that arises when a process *exits* during the probe window.
 *        (HAMi-core#264 describes this as an underflow; it is weaker than that.)
 *
 *   C2 — mergepid() (utils.c:52-71) copies only `.pid` and never
 *        `usedGpuMemory`, so the merged array's memory field is permanently
 *        zero.  That is why set_task_pid() reads context_size from the *raw*
 *        array at utils.c:162-169 while matching on the merged one.  With a
 *        single device polled the indices coincide by accident; removing the
 *        break desynchronises them.
 *
 * Both tests are expected to FAIL against current main.  They are written to
 * be submittable to HAMi-core on their own merits, and they are prerequisites
 * for any change that removes the single-device break.
 *
 * GPU-free: the functions under test touch no driver call.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/utils.h"
#include "include/log_utils.h"
#include "include/nvml_prefix.h"
#include <nvml.h>
#include "include/nvml_override.h"
#include "include/libcuda_hook.h"
#include "multiprocess/multiprocess_memory_limit.h"

int mergepid(unsigned int *prev, unsigned int *current,
             nvmlProcessInfo_t1 *sub, nvmlProcessInfo_t1 *merged);
int getextrapid(unsigned int prev, unsigned int current,
                nvmlProcessInfo_t1 *pre, nvmlProcessInfo_t1 *cur);

static int failures = 0, tests = 0;

static void report(const char *id, const char *name, int pass, const char *detail) {
    if (tests) printf(",\n");
    tests++;
    if (!pass) failures++;
    printf("    {\"id\": \"%s\", \"test\": \"%s\", \"result\": \"%s\",\n"
           "     \"detail\": \"%s\"}",
           id, name, pass ? "PASS" : "FAIL", detail);
}

int main(void) {
    printf("{\n");
    printf("  \"experiment\": \"X0c\",\n");
    printf("  \"baseline_commit\": \"5496322f2fb3e71bf1eca014fba3c9bc59ab8ffd\",\n");
    printf("  \"claims\": [\"C2\", \"C3\"],\n");
    printf("  \"note\": \"Both tests are EXPECTED TO FAIL on current main.\",\n");
    printf("  \"tests\": [\n");

    /* ---- C3: a process exits during the probe window ------------------ */
    {
        /* BEFORE: five processes on the device. */
        nvmlProcessInfo_t1 before[5] = {
            {1001, 100}, {1002, 100}, {1003, 100}, {1004, 100}, {1005, 100}
        };
        /* AFTER: three processes exited, and a *different container's* process
         * appeared in the same window.  Our own retain has not landed, so the
         * caller is still absent and the only correct answer is 0.
         *
         * The list shrank (3 < 5), which is exactly what the guard exists to
         * reject - but the comparison is unsigned, so it does not fire, and
         * the scan then returns the peer's PID as if it were ours. */
        nvmlProcessInfo_t1 after[3] = { {1001, 100}, {1002, 100}, {9999, 416} };

        unsigned int prev = 5, cur = 3;
        unsigned int got = (unsigned int)getextrapid(prev, cur, before, after);

        char d[256];
        snprintf(d, sizeof(d),
                 "prev=5 current=3 (three exited, peer 9999 appeared, caller "
                 "absent); expected 0, got %u", got);
        /* Passes only if the guard rejects current < prev. */
        report("C3", "getextrapid rejects a shrinking process list", got == 0, d);
    }

    /* ---- C3b: the guard's actual semantics --------------------------- */
    {
        unsigned int prev = 5, cur = 3;
        int unsigned_expr_le_zero = ((cur - prev) <= 0);
        int would_be_signed       = (((int)cur - (int)prev) <= 0);
        char d[256];
        snprintf(d, sizeof(d),
                 "(current-prev)<=0 evaluates %s as unsigned but %s as signed; "
                 "cur-prev=%u",
                 unsigned_expr_le_zero ? "TRUE" : "FALSE",
                 would_be_signed ? "TRUE" : "FALSE", cur - prev);
        report("C3b", "unsigned guard is equivalent to current==prev",
               unsigned_expr_le_zero == would_be_signed, d);
    }

    /* ---- C2: mergepid drops usedGpuMemory ---------------------------- */
    {
        nvmlProcessInfo_t1 sub[3] = {
            {2001, 96  * 1024ULL * 1024ULL},
            {2002, 128 * 1024ULL * 1024ULL},
            {2003, 416 * 1024ULL * 1024ULL},
        };
        nvmlProcessInfo_t1 merged[8];
        memset(merged, 0, sizeof(merged));

        unsigned int nsub = 3, nmerged = 0;
        mergepid(&nsub, &nmerged, sub, merged);

        int carried = 1;
        for (unsigned int i = 0; i < nmerged; i++)
            if (merged[i].usedGpuMemory != sub[i].usedGpuMemory) carried = 0;

        char d[256];
        snprintf(d, sizeof(d),
                 "merged %u entries; sub[0].usedGpuMemory=%llu but "
                 "merged[0].usedGpuMemory=%llu",
                 nmerged,
                 (unsigned long long)sub[0].usedGpuMemory,
                 (unsigned long long)merged[0].usedGpuMemory);
        report("C2", "mergepid carries usedGpuMemory into the merged array",
               carried, d);
    }

    /* ---- C2b: the index coincidence that the break relies on --------- */
    {
        /* Two devices' raw lists, with an overlapping PID - what polling
         * more than one device produces.  mergepid de-duplicates, so merged
         * index i no longer corresponds to raw index i. */
        nvmlProcessInfo_t1 raw[4] = {
            {3001, 10}, {3002, 20}, {3001, 10}, {3003, 30}
        };
        nvmlProcessInfo_t1 merged[8];
        memset(merged, 0, sizeof(merged));

        unsigned int nraw = 4, nmerged = 0;
        mergepid(&nraw, &nmerged, raw, merged);

        /* set_task_pid() matches on merged[i] then reads raw[i]. */
        int aligned = 1;
        for (unsigned int i = 0; i < nmerged; i++)
            if (merged[i].pid != raw[i].pid) aligned = 0;

        char d[320];
        snprintf(d, sizeof(d),
                 "raw=[3001,3002,3001,3003] merged=%u entries; "
                 "merged[2].pid=%d vs raw[2].pid=%d - set_task_pid would read "
                 "context_size from the wrong process",
                 nmerged, merged[2].pid, raw[2].pid);
        report("C2b", "merged and raw indices stay aligned across devices",
               aligned, d);
    }

    printf("\n  ],\n");
    printf("  \"summary\": {\"tests\": %d, \"failures\": %d, "
           "\"c2_c3_confirmed\": %s}\n",
           tests, failures, failures > 0 ? "true" : "false");
    printf("}\n");

    /* Exit 0 regardless: on current main these failures ARE the result. */
    return 0;
}
