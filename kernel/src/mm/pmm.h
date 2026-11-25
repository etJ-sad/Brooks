// SPDX-License-Identifier: GPL-2.0-only
// Physical page allocator. Only "Usable" memory is managed; bootloader
// reclaimable memory stays untouched for now (Limine page tables, stack, responses).
#pragma once
#include <stddef.h>
#include <stdint.h>

struct limine_memmap_response;

namespace mm {

constexpr uint64_t PAGE = 4096;

void init(limine_memmap_response* map, uint64_t hhdm_offset);
uint64_t hhdm();
template <typename T = void>
inline T* phys_to_virt(uint64_t phys) { return reinterpret_cast<T*>(phys + hhdm()); }
inline uint64_t virt_to_phys(const void* v) { return uint64_t(v) - hhdm(); }

// Returns physical address of `n` contiguous zeroed pages, 0 on failure.
uint64_t alloc_pages(size_t n);
void free_pages(uint64_t phys, size_t n);

uint64_t total_bytes();
uint64_t free_bytes();

}  // namespace mm
