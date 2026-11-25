// SPDX-License-Identifier: GPL-2.0-only
// UEFI runtime services. The bootloader (Brooks Boot or Limine) leaves the
// firmware in physical mode (no SetVirtualAddressMap), so every region marked
// EFI_MEMORY_RUNTIME is identity-mapped in the lower half and the services are
// called through their physical addresses with interrupts off.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace efi {

struct Time {
    uint16_t year;
    uint8_t month, day, hour, minute, second, pad1;
    uint32_t nanosecond;
    int16_t timezone;  // minutes from UTC, 2047 = unspecified (local time)
    uint8_t daylight, pad2;
};

struct Guid {
    uint32_t a;
    uint16_t b, c;
    uint8_t d[8];
};

enum ResetType : uint32_t { Cold = 0, Warm = 1, Shutdown = 2 };

// st_phys: physical address of the EFI system table; memmap: raw EFI memory
// map (virtual address) as handed over by the bootloader.
bool init(uint64_t st_phys, const void* memmap, uint64_t memmap_size, uint64_t desc_size, bool enabled);
bool available();
const char* status();  // why runtime services are off, or a summary line

bool get_time(Time& t);
// Only returns if the firmware refused (or runtime services are off).
void reset(ResetType type);

// Reads a variable; size in = buffer size, out = data size. Returns false if
// missing or too large.
bool get_variable(const char16_t* name, const Guid& guid, void* buf, size_t& size, uint32_t* attrs = nullptr);

// Ask the firmware to open its setup screen on the next boot
// (OsIndications.BOOT_TO_FW_UI). Returns false if not supported.
bool request_firmware_setup();
bool firmware_setup_supported();

// Writes /sys/efi/info.txt, /sys/efi/boot.txt and /sys/efi/variables.txt.
void publish_files();

}  // namespace efi
