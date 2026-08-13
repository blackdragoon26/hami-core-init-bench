/* stubs_cuda.c — aborting stubs for CUDA/NVML entry points reachable from
 * utils.o but NOT from getextrapid(). If any of these is called, the test's
 * premise is wrong and it crashes rather than reporting a bogus pass. */
#include <stdio.h>
#include <stdlib.h>
static void die(const char *fn){ fprintf(stderr,"FATAL: %s called; getextrapid() should not reach it\n",fn); abort(); }
int nvmlInit(void){ die("nvmlInit"); return 0; }
int nvmlDeviceGetCount(unsigned int*c){ (void)c; die("nvmlDeviceGetCount"); return 0; }
int cuDevicePrimaryCtxRetain(void*p,int d){ (void)p;(void)d; die("cuDevicePrimaryCtxRetain"); return 0; }
int cuDevicePrimaryCtxRelease_v2(int d){ (void)d; die("cuDevicePrimaryCtxRelease_v2"); return 0; }
int cuDeviceGetCount(int*c){ (void)c; die("cuDeviceGetCount"); return 0; }

/* data symbols owned by cuda/hook.c and libvgpu.c in the real build */
#define CUDA_DEVICE_MAX_COUNT 16
int cuda_to_nvml_map_array[CUDA_DEVICE_MAX_COUNT];
void *cuda_library_entry[1024];
