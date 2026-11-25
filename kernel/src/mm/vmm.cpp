// SPDX-License-Identifier: GPL-2.0-only
#include "mm/vmm.h"
#include "arch/x86_64/cpu.h"
#include "mm/pmm.h"

namespace vmm {
namespace {

constexpr uint64_t P = 1ull << 0, RW = 1ull << 1, PWT = 1ull << 3, PCD = 1ull << 4, PS = 1ull << 7,
                   NX = 1ull << 63;
constexpr uint64_t ADDR_MASK = 0x000ffffffffff000ull;
uint64_t g_next = 0xffffc00000000000ull;

uint64_t* table_at(uint64_t* parent, int idx) {
    uint64_t e = parent[idx];
    if (!(e & P)) {
        uint64_t t = mm::alloc_pages(1);
        if (!t) return nullptr;
        parent[idx] = t | P | RW;
        e = parent[idx];
    }
    if (e & PS) return nullptr;  // huge page in the way: not expected in our window
    return mm::phys_to_virt<uint64_t>(e & ADDR_MASK);
}

bool map_page(uint64_t va, uint64_t pa, uint64_t flags) {
    uint64_t* pml4 = mm::phys_to_virt<uint64_t>(cpu::read_cr3() & ADDR_MASK);
    uint64_t* pdpt = table_at(pml4, int((va >> 39) & 511));
    if (!pdpt) return false;
    uint64_t* pd = table_at(pdpt, int((va >> 30) & 511));
    if (!pd) return false;
    uint64_t* pt = table_at(pd, int((va >> 21) & 511));
    if (!pt) return false;
    pt[(va >> 12) & 511] = (pa & ADDR_MASK) | flags;
    asm volatile("invlpg (%0)" ::"r"(va) : "memory");
    return true;
}

}  // namespace

void* map(uint64_t phys, size_t len, Cache cache) {
    if (len == 0) return nullptr;
    uint64_t base = phys & ~(mm::PAGE - 1);
    uint64_t off = phys - base;
    uint64_t pages = (off + len + mm::PAGE - 1) / mm::PAGE;
    uint64_t va = g_next;
    const bool nxe = cpu::rdmsr(0xC0000080) & (1u << 11);  // EFER.NXE
    uint64_t flags = P | RW | (nxe ? NX : 0) | (cache == Uncached ? (PCD | PWT) : 0);
    for (uint64_t i = 0; i < pages; i++)
        if (!map_page(va + i * mm::PAGE, base + i * mm::PAGE, flags)) return nullptr;
    g_next += (pages + 1) * mm::PAGE;  // one guard page between mappings
    return reinterpret_cast<void*>(va + off);
}

bool map_identity(uint64_t phys, size_t len, Cache cache, bool exec) {
    uint64_t base = phys & ~(mm::PAGE - 1);
    uint64_t end = (phys + len + mm::PAGE - 1) & ~(mm::PAGE - 1);
    if (end > 0x0000800000000000ull) return false;
    const bool nxe = cpu::rdmsr(0xC0000080) & (1u << 11);
    uint64_t flags = P | RW | (nxe && !exec ? NX : 0) | (cache == Uncached ? (PCD | PWT) : 0);
    for (uint64_t a = base; a < end; a += mm::PAGE)
        if (!map_page(a, a, flags)) return false;
    return true;
}

}  // namespace vmm
