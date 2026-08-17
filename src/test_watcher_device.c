/* X0b — watcher device mismatch, from HAMi-core's real map functions.
 *
 * Claim under test:
 *   V11 — get_used_gpu_utilization() (multiprocess_utilization_watcher.c:212-221)
 *         iterates NVML device indices, converts each to a CUDA index, and then
 *         passes that CUDA index to nvmlDeviceGetHandleByIndex(), which takes an
 *         NVML index and performs no translation (nvml/hook.c:392-397).
 *
 * This models the loop faithfully using the project's own nvml_to_cuda_map() and
 * cuda_to_nvml_map(); it does not reimplement them.  For each NVML index the
 * watcher intends to sample, it reports the device it actually samples.
 *
 * Reference: set_task_pid() calls the same NVML function correctly, with the
 * NVML index (utils.c:118, :139).  The two call sites disagree.
 *
 * Falsifier: if intended == sampled for every case, V11 is withdrawn.
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

extern int g_phys_count;

struct tcase { const char *cvd; int phys; const char *note; };

static const struct tcase cases[] = {
    { "0",       2, "owns GPU 0 of 2" },
    { "1",       2, "owns GPU 1 of 2" },
    { "0,1",     2, "identity, 2 GPUs" },
    { "1,0",     2, "reversed, 2 GPUs" },
    { "0",       8, "owns GPU 0 of 8" },
    { "3",       8, "owns GPU 3 of 8" },
    { "2,1",     8, "descending, 8 GPUs" },
    { "0,1,2,3", 8, "identity prefix, 8 GPUs" },
};

int main(void) {
    int ncases = (int)(sizeof(cases) / sizeof(cases[0]));
    int total_iterations = 0, total_mismatches = 0, cases_with_mismatch = 0;

    printf("{\n");
    printf("  \"experiment\": \"X0b\",\n");
    printf("  \"baseline_commit\": \"5496322f2fb3e71bf1eca014fba3c9bc59ab8ffd\",\n");
    printf("  \"claim\": \"V11\",\n");
    printf("  \"model_of\": \"multiprocess_utilization_watcher.c:212-221\",\n");
    printf("  \"cases\": [\n");

    for (int t = 0; t < ncases; t++) {
        g_phys_count = cases[t].phys;
        setenv("CUDA_VISIBLE_DEVICES", cases[t].cvd, 1);
        parse_cuda_visible_env();

        int case_mismatch = 0;
        printf("    {\"cvd\": \"%s\", \"phys_devices\": %d, \"note\": \"%s\",\n",
               cases[t].cvd, cases[t].phys, cases[t].note);
        printf("     \"iterations\": [");

        int first = 1;
        /* Faithful model of the watcher loop. */
        for (int devi = 0; devi < cases[t].phys; devi++) {
            int cudadev = (int)nvml_to_cuda_map((unsigned int)devi);
            if (cudadev < 0) continue;          /* watcher.c:217-218 */

            /* watcher.c:221 — the CUDA index is passed to an NVML API, so the
             * device actually opened is NVML index `cudadev`. */
            int sampled  = cudadev;
            int intended = devi;
            int mismatch = (sampled != intended);

            total_iterations++;
            if (mismatch) { total_mismatches++; case_mismatch = 1; }

            printf("%s\n       {\"intended_nvml_dev\": %d, \"cuda_index\": %d, "
                   "\"sampled_nvml_dev\": %d, \"writes_to\": \"userutil[%d]\", "
                   "\"mismatch\": %s}",
                   first ? "" : ",", intended, cudadev, sampled, cudadev,
                   mismatch ? "true" : "false");
            first = 0;
        }
        if (case_mismatch) cases_with_mismatch++;
        printf("\n     ],\n     \"case_has_mismatch\": %s}%s\n",
               case_mismatch ? "true" : "false", (t == ncases - 1) ? "" : ",");
    }

    printf("  ],\n");
    printf("  \"summary\": {\n");
    printf("    \"cases\": %d,\n", ncases);
    printf("    \"loop_iterations\": %d,\n", total_iterations);
    printf("    \"mismatched_iterations\": %d,\n", total_mismatches);
    printf("    \"cases_with_mismatch\": %d,\n", cases_with_mismatch);
    printf("    \"v11_falsified\": %s\n", total_mismatches == 0 ? "true" : "false");
    printf("  }\n}\n");
    return 0;
}
