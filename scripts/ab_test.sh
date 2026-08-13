#!/usr/bin/env bash
# ab_test.sh — interleaved A/B of PR #253 (sweep only when slots are scarce).
# Both arms are built from the same tree; runs alternate to cancel drift.
set -u
cd /work
export CUDA_DEVICE_MEMORY_LIMIT=1024m
export CUDA_DEVICE_SM_LIMIT=50
export CUDA_DEVICE_MEMORY_SHARED_CACHE=/tmp/ab.cache

INC="-Ihami-core-rw/src -Ihami-core-rw -I$NV_INC/cuda_runtime/include -I$NV_INC/nvml_dev/include -Ibuild/config"

build_arm () {  # $1 = arm name, $2 = extra source dir
  gcc -c "$2/src/multiprocess/multiprocess_memory_limit.c" -o "build/mml_$1.o" \
      -I"$2/src" -I"$2" -I"$NV_INC/cuda_runtime/include" -I"$NV_INC/nvml_dev/include" \
      -Ibuild/config -D_GNU_SOURCE -fPIC -O2 2>/dev/null
  gcc build/bench.o "build/mml_$1.o" build/log_utils.o build/stubs.o \
      -o "build/bench_$1" -lpthread -lrt
}

build_arm base /work/hami-core-rw
build_arm pr253 /work/hami-core-pr253
echo "built both arms" >&2

echo "arm,N,repeat,wall_ms,p50_ms,p95_ms,max_ms,failed"
for n in 8 16 32 64 128 256 512; do
  for r in 1 2 3 4 5; do
    for arm in base pr253; do
      rm -f "$CUDA_DEVICE_MEMORY_SHARED_CACHE"
      out=$(./build/bench_$arm "$n" 2>/dev/null)
      w=$(echo "$out"  | sed 's/.*"wall_ms":\([0-9.]*\).*/\1/')
      p50=$(echo "$out"| sed 's/.*"p50_ms":\([0-9.]*\).*/\1/')
      p95=$(echo "$out"| sed 's/.*"p95_ms":\([0-9.]*\).*/\1/')
      mx=$(echo "$out" | sed 's/.*"max_ms":\([0-9.]*\).*/\1/')
      f=$(echo "$out"  | sed 's/.*"failed":\([0-9]*\).*/\1/')
      echo "$arm,$n,$r,$w,$p50,$p95,$mx,$f"
    done
  done
done
