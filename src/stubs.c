/*
 * stubs.c — minimal externals required to link HAMi-core's
 * multiprocess_memory_limit.o outside the full libvgpu.so.
 *
 * ensure_initialized() -> try_create_shrreg() + init_proc_slot_withlock() does
 * not call NVML or CUDA. That is a claim, so every NVML stub below aborts with
 * a diagnostic instead of returning a plausible value: if the claim is wrong,
 * this benchmark crashes loudly rather than silently reporting numbers that
 * came from a fake driver.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

static void die(const char *fn) {
    fprintf(stderr,
            "FATAL: %s was called on the ensure_initialized() path.\n"
            "The benchmark's premise (init path is NVML-free) is wrong.\n", fn);
    abort();
}

/* Referenced by nvml_get_device_memory_usage() and the utilisation paths.
   cuda_to_nvml_map is a pure index map in the real build, safe to provide. */
int cuda_to_nvml_map(int dev) { return dev; }
int nvml_to_cuda_map(int dev) { return dev; }

/* Not on the init path — abort if reached. */
int nvmlDeviceGetCount_v2(unsigned int *c) { (void)c; die("nvmlDeviceGetCount_v2"); return 0; }
int nvmlDeviceGetUUID(void *d, char *u, unsigned int l) { (void)d; (void)u; (void)l; die("nvmlDeviceGetUUID"); return 0; }
int nvmlDeviceGetHandleByIndex(unsigned int i, void *d) { (void)i; (void)d; die("nvmlDeviceGetHandleByIndex"); return 0; }
int nvmlDeviceGetComputeRunningProcesses(void *d, unsigned int *n, void *p) { (void)d; (void)n; (void)p; die("nvmlDeviceGetComputeRunningProcesses"); return 0; }
const char *nvmlErrorString(int r) { (void)r; die("nvmlErrorString"); return ""; }
int setspec(void) { die("setspec"); return 0; }

/* Utilisation watcher entry points, not on the init path. */
void init_utilization_watcher(void) { die("init_utilization_watcher"); }
void utilization_watcher(void) { die("utilization_watcher"); }
