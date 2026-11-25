#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Builds the NPU driver (kernel/src/npu) for the host against a register model
# and runs it: firmware answers / firmware stays silent / no firmware file.
set -euo pipefail
cd "$(dirname "$0")/../.."
FW=${1:-third_party/npu-fw/lib/firmware/intel/vpu/vpu_37xx_v1.bin}
OUT=${2:-build/npu-model}
mkdir -p "$OUT"
${CXX_HOST:-clang++} -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread \
    -Itests/npu-model/shim -Ikernel/src tests/npu-model/model.cpp \
    kernel/src/npu/npu.cpp kernel/src/npu/hw.cpp kernel/src/npu/mmu.cpp kernel/src/npu/boot.cpp -o "$OUT/model"
rc=0
for mode in 0 1 2; do
    if timeout 120 "$OUT/model" "$FW" $mode > "$OUT/mode$mode.txt" 2>&1; then
        echo "NPU model mode $mode: OK ($(tail -1 "$OUT/mode$mode.txt"))"
    else
        echo "NPU model mode $mode: FAILED"; cat "$OUT/mode$mode.txt"; rc=1
    fi
done
exit $rc
