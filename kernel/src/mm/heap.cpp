// SPDX-License-Identifier: GPL-2.0-only
// Kernel heap: small allocations (< 64 KiB) come from size classes carved out
// of pages; large allocations take whole page runs directly from the PMM.
#include "mm/heap.h"
#include "arch/x86_64/cpu.h"
#include "lib/string.h"
#include "mm/pmm.h"

namespace {

constexpr size_t HDR = 16;
constexpr uint32_t MAGIC_SMALL = 0x534D4C4C, MAGIC_LARGE = 0x4C524745;
constexpr int NCLASSES = 13;  // 16 .. 64 KiB
constexpr size_t kClass[NCLASSES] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};

struct Header {
    uint32_t magic;
    uint32_t cls;     // size class index, or page count for large blocks
    uint64_t size;    // requested size
};
static_assert(sizeof(Header) == HDR);

struct FreeNode {
    FreeNode* next;
};
FreeNode* g_free[NCLASSES];
size_t g_used = 0;

int class_for(size_t total) {
    for (int i = 0; i < NCLASSES; i++)
        if (total <= kClass[i]) return i;
    return -1;
}

bool refill(int c) {
    size_t sz = kClass[c];
    size_t pages = sz >= 16384 ? (sz * 4) / mm::PAGE : 4;  // 16 KiB slabs for small classes
    uint64_t phys = mm::alloc_pages(pages);
    if (!phys) return false;
    auto* base = mm::phys_to_virt<uint8_t>(phys);
    for (size_t off = 0; off + sz <= pages * mm::PAGE; off += sz) {
        auto* n = reinterpret_cast<FreeNode*>(base + off);
        n->next = g_free[c];
        g_free[c] = n;
    }
    return true;
}

}  // namespace

void* kmalloc(size_t n) {
    uint64_t f = cpu::save_irq();
    void* result = nullptr;
    size_t total = n + HDR;
    int c = class_for(total);
    if (c >= 0) {
        if (g_free[c] || refill(c)) {
            FreeNode* node = g_free[c];
            g_free[c] = node->next;
            auto* h = reinterpret_cast<Header*>(node);
            *h = {MAGIC_SMALL, uint32_t(c), n};
            g_used += kClass[c];
            result = reinterpret_cast<uint8_t*>(h) + HDR;
        }
    } else {
        size_t pages = (total + mm::PAGE - 1) / mm::PAGE;
        uint64_t phys = mm::alloc_pages(pages);
        if (phys) {
            auto* h = mm::phys_to_virt<Header>(phys);
            *h = {MAGIC_LARGE, uint32_t(pages), n};
            g_used += pages * mm::PAGE;
            result = reinterpret_cast<uint8_t*>(h) + HDR;
        }
    }
    cpu::restore_irq(f);
    return result;
}

void* kzalloc(size_t n) {
    void* p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void kfree(void* p) {
    if (!p) return;
    uint64_t f = cpu::save_irq();
    auto* h = reinterpret_cast<Header*>(static_cast<uint8_t*>(p) - HDR);
    if (h->magic == MAGIC_SMALL && h->cls < NCLASSES) {
        h->magic = 0;
        int c = int(h->cls);
        g_used -= kClass[c];
        auto* node = reinterpret_cast<FreeNode*>(h);
        node->next = g_free[c];
        g_free[c] = node;
    } else if (h->magic == MAGIC_LARGE) {
        h->magic = 0;
        g_used -= h->cls * mm::PAGE;
        mm::free_pages(mm::virt_to_phys(h), h->cls);
    }
    cpu::restore_irq(f);
}

void* krealloc(void* p, size_t n) {
    if (!p) return kmalloc(n);
    auto* h = reinterpret_cast<Header*>(static_cast<uint8_t*>(p) - HDR);
    size_t cap = h->magic == MAGIC_SMALL ? kClass[h->cls] - HDR : h->cls * mm::PAGE - HDR;
    if (n <= cap) {
        h->size = n;
        return p;
    }
    void* q = kmalloc(n);
    if (!q) return nullptr;
    memcpy(q, p, h->size);
    kfree(p);
    return q;
}

size_t heap_used() { return g_used; }

// C++ allocation operators.
void* operator new(size_t n) { return kmalloc(n); }
void* operator new[](size_t n) { return kmalloc(n); }
void operator delete(void* p) noexcept { kfree(p); }
void operator delete[](void* p) noexcept { kfree(p); }
void operator delete(void* p, size_t) noexcept { kfree(p); }
void operator delete[](void* p, size_t) noexcept { kfree(p); }
