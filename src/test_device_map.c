/* X0a — device-map truth table, derived from HAMi-core's real functions.
 *
 * Claims under test:
 *   V12 — cuda_to_nvml_map_array is initialised to identity for all
 *         CUDA_DEVICE_MAX_COUNT entries and only the first `count` are
 *         overwritten, so nvml_to_cuda_map() cannot report "not visible"
 *         for an unassigned device index below the array bound.
 *   C1  — set_task_pid() snapshots on the lowest *visible NVML* index while
 *         the probe context is created on CUDA device 0 (= CVD[0]).  The two
 *         diverge exactly when CVD[0] != min(CVD), which is a narrower
 *         condition than the one issue #225 states.
 *
 * Method: link the project's own parse_cuda_visible_env(), nvml_to_cuda_map()
 * and cuda_to_nvml_map() and drive them.  Nothing here reimplements the logic
 * under test; the device count is the only thing supplied by a stub.
 *
 * Falsifier for C1: if snapshot_nvml == probe_nvml for every CVD permutation,
 * C1 is wrong and #225's stated condition stands.
 * Falsifier for V12: if nvml_to_cuda_map() returns -1 for every unassigned
 * index, V12 is withdrawn.
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

int parse_cuda_visible_env(void);
unsigned int nvml_to_cuda_map(unsigned int nvmldev);
unsigned int cuda_to_nvml_map(unsigned int cudadev);
extern int cuda_to_nvml_map_array[CUDA_DEVICE_MAX_COUNT];

/* Physical NVML device count for the simulated node, defined in stubs.c.  NVML
 * does not honour CUDA_VISIBLE_DEVICES, so this stays at the node's true device
 * count while cuDeviceGetCount() reports only the visible ones. */
extern int g_phys_count;

struct tcase { const char *cvd; int phys; const char *note; };

static const struct tcase cases[] = {
    { "0",       2, "single device, index 0" },
    { "1",       2, "single device, offset - the discriminating case" },
    { "0,1",     2, "identity" },
    { "1,0",     2, "reversed" },
    /* The four combinations maverick123123 used to verify PR #230 on an
     * 8x RTX 3090 (comment on #225).  All are within devices {0,1}. */
    { "0",       8, "8-GPU: verified by maintainer" },
    { "1",       8, "8-GPU: verified by maintainer" },
    { "0,1",     8, "8-GPU: verified by maintainer" },
    { "1,0",     8, "8-GPU: verified by maintainer" },
    /* Single devices with index >= 2 - not in that matrix. */
    { "2",       8, "8-GPU: NOT in the verification matrix" },
    { "3",       8, "8-GPU: NOT in the verification matrix" },
    { "7",       8, "8-GPU: NOT in the verification matrix" },
    { "1,2",     8, "ascending, does not start at 0" },
    { "2,1",     8, "descending" },
    { "3,0,1",   8, "CVD[0] is not the minimum" },
    { "0,1,2,3", 8, "ascending from 0" },
    { "7,6,5,4", 8, "fully reversed" },
};

int main(void) {
    int ncases = (int)(sizeof(cases) / sizeof(cases[0]));

    printf("{\n");
    printf("  \"experiment\": \"X0a\",\n");
    printf("  \"baseline_commit\": \"5496322f2fb3e71bf1eca014fba3c9bc59ab8ffd\",\n");
    printf("  \"claims\": [\"V12\", \"C1\"],\n");
    printf("  \"cuda_device_max_count\": %d,\n", CUDA_DEVICE_MAX_COUNT);
    printf("  \"cases\": [\n");

    int c1_divergences = 0, v12_phantoms = 0;

    for (int t = 0; t < ncases; t++) {
        g_phys_count = cases[t].phys;
        setenv("CUDA_VISIBLE_DEVICES", cases[t].cvd, 1);

        int nvis = parse_cuda_visible_env();

        /* Which NVML index would set_task_pid() snapshot?  utils.c:113-129
         * walks NVML indices and breaks at the first one that maps to a CUDA
         * device. */
        int snapshot = -1;
        for (int i = 0; i < cases[t].phys; i++) {
            if ((int)nvml_to_cuda_map((unsigned int)i) >= 0) { snapshot = i; break; }
        }

        /* On main the probe is hardcoded to CUDA device 0 (utils.c:133). */
        int probe = (int)cuda_to_nvml_map(0);

        /* Under PR #230 the probe instead uses probeDev = cudaDev taken from
         * the snapshot loop, which makes snapshot and probe consistent.  But
         * the fix trusts nvml_to_cuda_map(), and V12 lets that return a CUDA
         * index for a device the container was never assigned.  The CUDA
         * driver only exposes as many devices as CVD lists, so a probeDev at
         * or beyond that count is CUDA_ERROR_INVALID_DEVICE (101). */
        int probe_dev_230 = (snapshot >= 0) ? (int)nvml_to_cuda_map((unsigned int)snapshot) : -1;
        int cuda_visible = 0;
        for (const char *p = cases[t].cvd; p; ) {
            cuda_visible++;
            p = strchr(p, ',');
            if (p) p++;
        }
        const char *verdict_230 =
            (probe_dev_230 < 0)            ? "no mapped device" :
            (probe_dev_230 >= cuda_visible) ? "CUDA_ERROR_INVALID_DEVICE" : "OK";

        /* V12: an NVML index that is NOT listed in CVD but still resolves. */
        int phantom_count = 0;
        char phantoms[256] = {0};
        for (int i = 0; i < cases[t].phys; i++) {
            if ((int)nvml_to_cuda_map((unsigned int)i) < 0) continue;
            int listed = 0;
            const char *p = cases[t].cvd;
            while (p && *p) {
                if (atoi(p) == i) { listed = 1; break; }
                p = strchr(p, ',');
                if (p) p++;
            }
            if (!listed) {
                char b[16];
                snprintf(b, sizeof(b), "%s%d", phantom_count ? "," : "", i);
                strncat(phantoms, b, sizeof(phantoms) - strlen(phantoms) - 1);
                phantom_count++;
            }
        }
        if (phantom_count) v12_phantoms++;

        int diverges = (snapshot != probe);
        if (diverges) c1_divergences++;

        printf("    {\"cvd\": \"%s\", \"phys_devices\": %d, \"note\": \"%s\",\n",
               cases[t].cvd, cases[t].phys, cases[t].note);
        printf("     \"parsed_entries\": %d, \"map\": [", nvis);
        for (int i = 0; i < cases[t].phys; i++)
            printf("%s%d", i ? "," : "", cuda_to_nvml_map_array[i]);
        printf("],\n");
        printf("     \"snapshot_nvml_dev\": %d, \"probe_nvml_dev\": %d,\n", snapshot, probe);
        printf("     \"predicted_main\": \"%s\",\n", diverges ? "host pid is error!" : "OK");
        printf("     \"pr230_probeDev\": %d, \"cuda_visible_devices\": %d, "
               "\"predicted_pr230\": \"%s\",\n",
               probe_dev_230, cuda_visible, verdict_230);
        printf("     \"v12_phantom_visible_devices\": [%s]}%s\n",
               phantoms, (t == ncases - 1) ? "" : ",");
    }

    printf("  ],\n");
    printf("  \"summary\": {\n");
    printf("    \"cases\": %d,\n", ncases);
    printf("    \"c1_divergent_cases\": %d,\n", c1_divergences);
    printf("    \"v12_cases_with_phantom_devices\": %d,\n", v12_phantoms);
    printf("    \"c1_falsified\": %s,\n", c1_divergences == 0 ? "true" : "false");
    printf("    \"v12_falsified\": %s\n", v12_phantoms == 0 ? "true" : "false");
    printf("  }\n}\n");
    return 0;
}
