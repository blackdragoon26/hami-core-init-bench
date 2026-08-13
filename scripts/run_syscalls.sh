#!/usr/bin/env bash
# run_syscalls.sh — count /proc/<pid>/stat opens performed by the whole process
# group during a cold concurrent init. This measures algorithmic work only and is
# immune to CPU oversubscription, unlike wall-clock timing.
set -u
cd /work
export CUDA_DEVICE_MEMORY_LIMIT=1024m
export CUDA_DEVICE_SM_LIMIT=50
export CUDA_DEVICE_MEMORY_SHARED_CACHE=/tmp/bench_shr.cache

echo "N,proc_stat_opens,total_openat,lockf_fcntl"
for n in 1 2 4 8 16 32 64 128; do
  rm -f "$CUDA_DEVICE_MEMORY_SHARED_CACHE" /tmp/st.$n
  strace -ff -qq -e trace=openat,fcntl -o /tmp/st.$n ./build/bench_shrreg "$n" >/dev/null 2>&1
  stat_opens=$(cat /tmp/st.$n.* 2>/dev/null | grep -c 'openat.*"/proc/[0-9]*/stat"' || true)
  all_openat=$(cat /tmp/st.$n.* 2>/dev/null | grep -c 'openat' || true)
  fcntls=$(cat /tmp/st.$n.* 2>/dev/null | grep -c 'fcntl' || true)
  echo "$n,$stat_opens,$all_openat,$fcntls"
  rm -f /tmp/st.$n.*
done
