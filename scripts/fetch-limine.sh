#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Fetch prebuilt Limine (UEFI loader) at a pinned commit into third_party/limine.
set -euo pipefail
cd "$(dirname "$0")/.."

LIMINE_BRANCH="v9.x-binary"
LIMINE_COMMIT="ee5d29cd0a8034612dcd1df3f00052480db785c5"
DEST="third_party/limine"

if [[ -f "$DEST/BOOTX64.EFI" ]] && [[ "$(git -C "$DEST" rev-parse HEAD 2>/dev/null)" == "$LIMINE_COMMIT" ]]; then
    exit 0
fi
rm -rf "$DEST"
mkdir -p third_party
git clone --quiet --branch "$LIMINE_BRANCH" --single-branch https://github.com/limine-bootloader/limine.git "$DEST"
git -C "$DEST" -c advice.detachedHead=false checkout --quiet "$LIMINE_COMMIT"
echo "Limine $LIMINE_BRANCH @ ${LIMINE_COMMIT:0:12} ready in $DEST"
