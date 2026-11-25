// SPDX-License-Identifier: GPL-2.0-only
// The NPU's own IOMMU (an SMMUv3 subset at BAR0+0x200000) and the page tables
// of the global context (SSID 0), after Linux ivpu_mmu.c / ivpu_mmu_context.c.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "npu/hw.h"

namespace npu::mmu {

// Allocates stream table, context descriptors, queues, the global and the
// reserved context's root tables, then enables the MMU.
bool init();

// Maps buffer b at NPU address vpu (4 KiB pages). `snooped` = LLC coherent.
bool map(hw::Buf& b, uint64_t vpu, bool snooped);
// Write back the page tables (the table walker does not snoop) and invalidate the TLB.
bool commit();

// Prints the global error register and any events (translation faults etc.).
void report();

// Translates an NPU address through the global context (for checks), 0 if unmapped.
uint64_t translate(uint64_t vpu);

}  // namespace npu::mmu
