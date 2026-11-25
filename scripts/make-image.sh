#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build a raw GPT disk image with one FAT32 EFI System Partition:
#   /EFI/BOOT/BOOTX64.EFI        Limine (removable-media path, no NVRAM entry needed)
#   /boot/limine/limine.conf
#   /boot/kernel.elf
# Usage: make-image.sh <kernel.elf> <out.img>
# The image can be written with dd or Rufus (DD mode) to a USB stick / USB-SSD.
set -euo pipefail
cd "$(dirname "$0")/.."

KERNEL="$1"
OUT="$2"
SIZE_MB="${IMG_SIZE_MB:-64}"
LABEL="${IMG_LABEL:-AXONOS}"

for t in sgdisk mformat mmd mcopy; do
    command -v "$t" >/dev/null || { echo "missing tool: $t (apt install gdisk mtools)" >&2; exit 1; }
done

rm -f "$OUT"
truncate -s "${SIZE_MB}M" "$OUT"
sgdisk --clear --new=1:2048:0 --typecode=1:ef00 --change-name=1:"EFI system partition" "$OUT" >/dev/null

PART="$OUT@@1M"   # partition starts at LBA 2048 = 1 MiB
mformat -i "$PART" -F -v "$LABEL" ::
mmd -i "$PART" ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
mcopy -i "$PART" third_party/limine/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$PART" boot/limine.conf ::/boot/limine/limine.conf
mcopy -i "$PART" "$KERNEL" ::/boot/kernel.elf

echo "Image: $OUT (${SIZE_MB} MiB, GPT + FAT32 ESP)"
mdir -i "$PART" -/ :: | grep -E 'EFI|boot|BOOTX64|limine|kernel' || true
