// SPDX-License-Identifier: GPL-2.0-only
// Kernel virtual memory on top of Limine's page tables: maps device MMIO and
// firmware tables into a dedicated window (0xffffc00000000000 upwards).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace vmm {

enum Cache { WriteBack, Uncached };

// Map [phys, phys+len) and return the virtual address of `phys`, nullptr on failure.
void* map(uint64_t phys, size_t len, Cache cache);

// Map [phys, phys+len) at the same virtual address (lower half). Used for UEFI
// runtime services, which are called without SetVirtualAddressMap.
bool map_identity(uint64_t phys, size_t len, Cache cache, bool exec);

}  // namespace vmm
