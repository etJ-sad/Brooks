// SPDX-License-Identifier: GPL-2.0-only
// Minimal ACPI table access (no AML interpreter yet; ACPICA comes later).
#pragma once
#include <stdint.h>

namespace acpi {

struct IoApic {
    uint8_t id;
    uint32_t addr;
    uint32_t gsi_base;
};

struct Override {  // legacy ISA IRQ -> GSI
    uint8_t irq;
    uint32_t gsi;
    uint16_t flags;  // MPS INTI flags: bits 0-1 polarity, 2-3 trigger
};

struct Info {
    bool ok = false;
    uint8_t revision = 0;
    char oem[7] = {};
    uint64_t lapic_addr = 0xFEE00000;
    IoApic ioapic[8];
    int n_ioapic = 0;
    Override iso[16];
    int n_iso = 0;
    int n_cpus = 0;
    bool has_8042 = true;  // FADT IAPC_BOOT_ARCH bit 1 (only trusted if FADT rev >= 2)
    bool fadt_8042_flag_valid = false;
    bool has_mcfg = false;
    uint64_t ecam_base = 0;
    int n_tables = 0;
    char tables[48][5] = {};
};

bool init(uint64_t rsdp_phys);
const Info& info();
// Returns the mapped table with the given 4-letter signature, or nullptr.
const void* find(const char* sig);

[[noreturn]] void reboot();
// Tries ACPI S5 (soft off). Returns only if it failed.
void poweroff();

}  // namespace acpi
