// SPDX-License-Identifier: GPL-2.0-only
// Internal helpers shared by the NPU driver files: register access through a
// few mapped BAR0 windows, logging, delays, and DMA memory the NPU can reach.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace npu::hw {

// ---- logging (collected into /sys/npu/bringup.txt as well)
__attribute__((format(printf, 1, 2))) void out(const char* fmt, ...);

// ---- registers. BAR0 (NPU IP) is reached through windows mapped by
// map_bars(); an offset outside every window reads 0xffffffff and is logged.
bool map_bars(uint64_t bar0, uint64_t bar4);
uint32_t rv(uint32_t off);
void wv(uint32_t off, uint32_t v);
uint64_t rv64(uint32_t off);
void wv64(uint32_t off, uint64_t v);
uint32_t rb(uint32_t off);  // buttress (BAR4)
void wb(uint32_t off, uint32_t v);

void udelay(uint64_t us);
uint32_t fld(uint32_t v, uint32_t mask);
// Waits until field `mask` of a register equals `want` (polls every 10 us).
bool poll(bool btrs, uint32_t reg, uint32_t mask, uint32_t want, uint64_t timeout_us, const char* what);

// ---- memory shared with the NPU. Physically contiguous, zeroed, mapped
// write-back on the CPU side. The NPU's page-table walker does not snoop the
// CPU caches, and neither do non-coherent NPU mappings, so the CPU writes
// back (flush) after writing and before reading.
struct Buf {
    uint64_t phys = 0;
    uint8_t* cpu = nullptr;
    uint64_t vpu = 0;  // NPU virtual address (0 if not mapped)
    size_t size = 0;   // bytes, page multiple
};
bool alloc(Buf& b, size_t size);
void free_all();  // frees every Buf from alloc(); only once the NPU is reset
size_t allocated_bytes();
void flush(const void* p, size_t len);  // clflush each line, then mfence

}  // namespace npu::hw
