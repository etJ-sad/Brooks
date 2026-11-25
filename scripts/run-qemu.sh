#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Boot the disk image in QEMU with OVMF (UEFI). Serial log goes to stdout.
# Usage: run-qemu.sh <image> [extra qemu args...]
#   HEADLESS=1   no window (for CI/servers), screenshot via QEMU monitor if needed
#   RES=3840x2160  emulate a 4K GOP mode (default 1920x1080)
set -euo pipefail

IMG="$1"; shift || true

find_fw() {
    for f in "$@"; do [[ -f "$f" ]] && { echo "$f"; return; }; done
}
CODE=$(find_fw /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/x64/OVMF_CODE.4m.fd \
    /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/qemu/edk2-x86_64-code.fd /opt/homebrew/share/qemu/edk2-x86_64-code.fd)
VARS=$(find_fw /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd /usr/share/edk2/x64/OVMF_VARS.4m.fd \
    /usr/share/edk2/ovmf/OVMF_VARS.fd /usr/share/qemu/edk2-i386-vars.fd /opt/homebrew/share/qemu/edk2-i386-vars.fd)
[[ -n "$CODE" && -n "$VARS" ]] || { echo "OVMF not found (apt install ovmf)" >&2; exit 1; }

mkdir -p build
cp -f "$VARS" build/ovmf-vars.fd

RES="${RES:-1920x1080}"
XRES="${RES%x*}"; YRES="${RES#*x}"

DISPLAY_ARGS=()
if [[ "${HEADLESS:-0}" == "1" ]]; then
    DISPLAY_ARGS=(-display none)
fi

ACCEL=(-cpu max)
if [[ -w /dev/kvm ]]; then ACCEL=(-enable-kvm -cpu host); fi

exec qemu-system-x86_64 \
    -machine q35 "${ACCEL[@]}" -m 2G -smp 4 \
    -drive if=pflash,format=raw,unit=0,file="$CODE",readonly=on \
    -drive if=pflash,format=raw,unit=1,file=build/ovmf-vars.fd \
    -drive if=none,id=usbdisk,format=raw,file="$IMG" \
    -device qemu-xhci,id=xhci -device usb-storage,bus=xhci.0,drive=usbdisk,bootindex=0 \
    -device VGA,xres="$XRES",yres="$YRES",vgamem_mb=64 \
    -serial stdio -no-reboot \
    "${DISPLAY_ARGS[@]}" "$@"
