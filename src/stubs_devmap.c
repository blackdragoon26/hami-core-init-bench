/* Link stubs for X0a.
 *
 * Every symbol here is either (a) a device-count oracle that models the one
 * real asymmetry the experiment depends on, or (b) an abort-on-call trap, so
 * that if the compiled path ever reaches code it should not, the run fails
 * loudly instead of returning a plausible value.  Same discipline as the
 * existing GPU-free harness.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* nvml_prefix.h supplies entry_t and the nvml_library_entry declaration.
 * libnvml_hook.h is deliberately NOT included: it pulls in HAMi-core's vendored
 * nvml-subset.h, which redefines types already provided by the real nvml.h from
 * nvidia-nvml-dev-cu12. */
#include "include/nvml_prefix.h"
#include <nvml.h>
#include "include/nvml_override.h"
#include "include/libcuda_hook.h"

/* Physical NVML device count for the simulated node.  Set by the driver. */
int g_phys_count = 2;

int   g_log_level = 0;      /* silence LOG_INFO/DEBUG; ERROR still prints */
FILE *fp1 = NULL;
size_t context_size = 0;
size_t GIT_HASH_5496322 = 0;
size_t GIT_BRANCH_main = 0;

void log_utils_init(void) {}

static void die(const char *fn) {
    fprintf(stderr,
            "FATAL: %s was called on the device-map path.\n"
            "X0a's premise (map resolution touches no driver call beyond the "
            "device counts) is wrong.\n", fn);
    abort();
}

/* --- the two device counts, which is the whole point ------------------ */

/* NVML reports every physical device; it does not honour
 * CUDA_VISIBLE_DEVICES. */
nvmlReturn_t nvmlDeviceGetCount_v2(unsigned int *c) { *c = (unsigned int)g_phys_count; return NVML_SUCCESS; }
nvmlReturn_t nvmlDeviceGetCount(unsigned int *c)    { *c = (unsigned int)g_phys_count; return NVML_SUCCESS; }

/* The CUDA driver *does* honour it, reporting only the visible devices. */
static CUresult stub_cuDeviceGetCount(int *count) {
    const char *s = getenv("CUDA_VISIBLE_DEVICES");
    if (s == NULL || strlen(s) == 0) { *count = g_phys_count; return CUDA_SUCCESS; }
    int n = 1;
    for (size_t i = 0; i < strlen(s); i++) if (s[i] == ',') n++;
    *count = n;
    return CUDA_SUCCESS;
}

static CUresult trap_cuda(void) { die("an unexpected CUDA entry"); return CUDA_ERROR_UNKNOWN; }

cuda_entry_t cuda_library_entry[] = {
    [OVERRIDE_cuDeviceGetCount] = { .fn_ptr = (void *)stub_cuDeviceGetCount,
                                    .name   = "cuDeviceGetCount" },
    [OVERRIDE_cuInit]           = { .fn_ptr = (void *)trap_cuda, .name = "cuInit" },
};

entry_t nvml_library_entry[] = {
    { .fn_ptr = NULL, .name = "unused" },
};
