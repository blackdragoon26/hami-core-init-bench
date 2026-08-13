#!/usr/bin/env bash
# run_sweep.sh — concurrency sweep over HAMi-core's real ensure_initialized().
set -u

cd /work
export CUDA_DEVICE_MEMORY_LIMIT=1024m
export CUDA_DEVICE_SM_LIMIT=50
export CUDA_DEVICE_MEMORY_SHARED_CACHE=/tmp/bench_shr.cache

REPEATS="${REPEATS:-5}"
POINTS="${POINTS:-1 2 4 8 16 32 64 128 256 512}"

echo "# HAMi-core shared-region init sweep"
echo "# commit: $(cd hami-core-rw && git rev-parse --short HEAD)"
echo "# kernel: $(uname -r)  arch: $(uname -m)  nproc: $(nproc)"
echo "# repeats per point: $REPEATS"
echo "#"

for n in $POINTS; do
  for r in $(seq 1 "$REPEATS"); do
    rm -f "$CUDA_DEVICE_MEMORY_SHARED_CACHE"
    out=$(./build/bench_shrreg "$n" 2>/dev/null)
    echo "${out%\}}, \"repeat\":$r}"
  done
done
