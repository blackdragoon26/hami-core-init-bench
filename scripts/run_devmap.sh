#!/usr/bin/env bash
# run_devmap.sh — device-mapping experiments against HAMi-core's REAL map functions.
#
# The other benchmarks in this repo link src/stubs.c, which provides
#   int cuda_to_nvml_map(int dev) { return dev; }
#   int nvml_to_cuda_map(int dev) { return dev; }
# as identity. That is fine for the shared-region paths, which only use them for
# indexing — but it makes the device-mapping logic itself invisible.
#
# These three tests instead compile HAMi-core's own multiprocess_utilization_watcher.c
# and utils.c, so parse_cuda_visible_env(), nvml_to_cuda_map() and cuda_to_nvml_map()
# under test are the project's real implementations. The only thing supplied by a stub
# is the device count, which models the one asymmetry that matters:
#
#   the CUDA driver honours CUDA_VISIBLE_DEVICES; NVML does not.
#
# Every other symbol is an abort() trap, so if resolution ever reaches a driver call
# the run fails loudly instead of returning a plausible value.
#
# No GPU required.
set -euo pipefail
cd /work

: "${NV_INC:=/usr/local/lib/python3.10/dist-packages/nvidia}"
SRC=hami-core-rw
INC="-I$SRC/src -I$SRC -I$NV_INC/cuda_runtime/include -I$NV_INC/nvml_dev/include -Ibuild/config"

if [ ! -f "$SRC/src/utils.c" ]; then
  echo "error: HAMi-core source not found at $SRC/" >&2
  echo "  git clone https://github.com/Project-HAMi/HAMi-core.git $SRC" >&2
  echo "  (cd $SRC && git checkout 5496322)" >&2
  exit 1
fi

mkdir -p build/config build results

# static_config.h is normally emitted by HAMi-core's cmake step; the two git
# symbols are referenced by the header and never read on these paths.
if [ ! -f build/config/static_config.h ]; then
  cat > build/config/static_config.h <<'EOF'
#define MULTIPROCESS_LIMIT_ENABLE
#define HOOK_NVML_ENABLE
extern size_t GIT_HASH_5496322;
extern size_t GIT_BRANCH_main;
EOF
fi

echo "building against $(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo unknown)"

gcc -c "$SRC/src/utils.c" -o build/dm_utils.o $INC -D_GNU_SOURCE -fPIC -O2 -w
gcc -c "$SRC/src/multiprocess/multiprocess_utilization_watcher.c" \
    -o build/dm_watcher.o $INC -D_GNU_SOURCE -fPIC -O2 -w
gcc -c src/stubs_devmap.c      -o build/dm_stubs.o      $INC -D_GNU_SOURCE -O2 -w
gcc -c src/stubs_devmap_trap.c -o build/dm_stubs_trap.o -D_GNU_SOURCE -O2 -w

build_and_run () {  # $1 = source basename, $2 = output json
  gcc -c "src/$1.c" -o "build/$1.o" $INC -D_GNU_SOURCE -O2 -w
  gcc "build/$1.o" build/dm_utils.o build/dm_watcher.o \
      build/dm_stubs.o build/dm_stubs_trap.o -o "build/$1" -lpthread -lrt
  "./build/$1" > "results/$2"
  echo "  results/$2"
}

echo "running:"
build_and_run test_device_map     device_map_truth_table.json
build_and_run test_watcher_device watcher_device_mismatch.json
build_and_run test_guard_merge    guard_and_merge_tests.json

echo
echo "summaries:"
for f in device_map_truth_table watcher_device_mismatch guard_and_merge_tests; do
  printf '  %-30s ' "$f"
  python3 -c "import json,sys; print(json.load(open('results/$f.json'))['summary'])"
done
