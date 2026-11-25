#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Fetch the Intel NPU firmware for Meteor Lake / Arrow Lake (vpu_37xx_v1.bin)
# and its license from linux-firmware at a pinned tag, check the SHA-256 and
# stage it under third_party/npu-fw/lib/firmware for the initrd.
# The firmware is Intel's redistributable binary (see LICENSE.intel_vpu); it is
# not part of this repository.
set -euo pipefail
cd "$(dirname "$0")/.."

TAG="20261124"
BASE="https://gitlab.com/kernel-firmware/linux-firmware/-/raw/$TAG"
DEST="third_party/npu-fw/lib/firmware"
FW_SHA256="5e9e0460ec72120138adc209abb6dda46ac465172b1fc914039890083055d8aa"
LIC_SHA256="4a10165191a9ab82daf1539cab4242ac266eeaa1ad1d158d5d9699546966c139"

ok() { [[ -f "$1" ]] && echo "$2  $1" | sha256sum --quiet -c - 2>/dev/null; }

fetch() {  # url file sha256
    if ok "$2" "$3"; then return 0; fi
    mkdir -p "$(dirname "$2")"
    curl -sSfL --retry 4 -o "$2.part" "$1"
    if ! echo "$3  $2.part" | sha256sum --quiet -c -; then
        echo "checksum mismatch for $1" >&2
        exit 1
    fi
    mv "$2.part" "$2"
}

fetch "$BASE/intel/vpu/vpu_37xx_v1.bin" "$DEST/intel/vpu/vpu_37xx_v1.bin" "$FW_SHA256"
fetch "$BASE/LICENSES/LICENSE.intel_vpu" "$DEST/LICENSE.intel_vpu" "$LIC_SHA256"
echo "NPU firmware (linux-firmware $TAG) ready in $DEST"
