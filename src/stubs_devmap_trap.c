/* Abort-traps for X0a.
 *
 * Deliberately includes none of HAMi-core's headers: these exist only to
 * satisfy the linker for code paths X0a must never execute.  If the device-map
 * resolution ever reaches one, the run aborts with a diagnostic rather than
 * returning a plausible value and quietly invalidating the result.
 *
 * This is the same discipline as the existing GPU-free harness's stubs.c,
 * where no abort has ever fired.
 */
#include <stdio.h>
#include <stdlib.h>

static int die(const char *fn) {
    fprintf(stderr,
            "FATAL: %s was called during device-map resolution.\n"
            "X0a's premise is wrong: the map path is not free of driver and "
            "shared-region calls.\n", fn);
    abort();
    return 0;
}

/* Shared-region entry points */
int  ensure_initialized(void)        { return die("ensure_initialized"); }
int  lock_shrreg(void)               { return die("lock_shrreg"); }
int  unlock_shrreg(void)             { return die("unlock_shrreg"); }
void *find_proc_by_hostpid(int p)    { (void)p; die("find_proc_by_hostpid"); return 0; }
int  set_host_pid(int p)             { (void)p; return die("set_host_pid"); }
int  update_host_pid(void)           { return die("update_host_pid"); }
int  get_utilization_switch(void)    { return die("get_utilization_switch"); }
int  get_recent_kernel(void)         { return die("get_recent_kernel"); }
int  set_recent_kernel(int v)        { (void)v; return die("set_recent_kernel"); }
int  get_current_device_sm_limit(int d) { (void)d; return die("get_current_device_sm_limit"); }
int  init_gpu_device_utilization(void)  { return die("init_gpu_device_utilization"); }

/* Owned by multiprocess_memory_limit.c */
int pidfound = 0;

/* CUDA driver */
int cuDeviceGet(void)                { return die("cuDeviceGet"); }
int cuDeviceGetAttribute(void)       { return die("cuDeviceGetAttribute"); }
int cuCtxGetDevice(void)             { return die("cuCtxGetDevice"); }
int cuDevicePrimaryCtxRetain(void)     { return die("cuDevicePrimaryCtxRetain"); }
/* Real cuda.h maps cuDevicePrimaryCtxRelease -> _v2, so both are needed. */
int cuDevicePrimaryCtxRelease(void)    { return die("cuDevicePrimaryCtxRelease"); }
int cuDevicePrimaryCtxRelease_v2(void) { return die("cuDevicePrimaryCtxRelease_v2"); }

/* NVML */
int nvmlInit(void)                             { return die("nvmlInit"); }
int nvmlDeviceGetHandleByIndex(void)           { return die("nvmlDeviceGetHandleByIndex"); }
int nvmlDeviceGetComputeRunningProcesses(void) { return die("nvmlDeviceGetComputeRunningProcesses"); }
int nvmlDeviceGetProcessUtilization(void)      { return die("nvmlDeviceGetProcessUtilization"); }
int nvmlDeviceGetProcessesUtilizationInfo(void){ return die("nvmlDeviceGetProcessesUtilizationInfo"); }
