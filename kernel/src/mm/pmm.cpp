// SPDX-License-Identifier: GPL-2.0-only
#include "mm/pmm.h"
#include "lib/string.h"
#include <limine.h>

namespace mm {
namespace {

// Free memory is a sorted list of [base, base+pages) runs stored in a static
// table. Allocation is first fit; freeing merges with neighbours.
constexpr int MAX_RUNS = 1024;
struct Run {
    uint64_t base, pages;
};
Run g_runs[MAX_RUNS];
int g_nruns = 0;
uint64_t g_hhdm = 0;
uint64_t g_total = 0, g_free = 0;

void remove_run(int i) {
    for (int k = i; k < g_nruns - 1; k++) g_runs[k] = g_runs[k + 1];
    g_nruns--;
}

bool insert_run(int i, uint64_t base, uint64_t pages) {
    if (g_nruns >= MAX_RUNS) return false;
    for (int k = g_nruns; k > i; k--) g_runs[k] = g_runs[k - 1];
    g_runs[i] = {base, pages};
    g_nruns++;
    return true;
}

void add_free(uint64_t base, uint64_t pages) {
    int i = 0;
    while (i < g_nruns && g_runs[i].base < base) i++;
    bool merge_prev = i > 0 && g_runs[i - 1].base + g_runs[i - 1].pages * PAGE == base;
    bool merge_next = i < g_nruns && base + pages * PAGE == g_runs[i].base;
    if (merge_prev && merge_next) {
        g_runs[i - 1].pages += pages + g_runs[i].pages;
        remove_run(i);
    } else if (merge_prev) {
        g_runs[i - 1].pages += pages;
    } else if (merge_next) {
        g_runs[i].base = base;
        g_runs[i].pages += pages;
    } else if (!insert_run(i, base, pages)) {
        return;  // table full: leak the range rather than corrupt state
    }
    g_free += pages * PAGE;
}

}  // namespace

void init(limine_memmap_response* map, uint64_t hhdm_offset) {
    g_hhdm = hhdm_offset;
    if (!map) return;
    for (uint64_t i = 0; i < map->entry_count; i++) {
        auto* e = map->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t b = (e->base + PAGE - 1) & ~(PAGE - 1);
        uint64_t end = (e->base + e->length) & ~(PAGE - 1);
        if (b < 0x100000) b = 0x100000;  // keep low memory free for future AP trampolines
        if (end <= b) continue;
        add_free(b, (end - b) / PAGE);
        g_total += end - b;
    }
}

uint64_t hhdm() { return g_hhdm; }

uint64_t alloc_pages(size_t n) {
    if (n == 0) return 0;
    for (int i = 0; i < g_nruns; i++) {
        if (g_runs[i].pages < n) continue;
        uint64_t p = g_runs[i].base;
        g_runs[i].base += n * PAGE;
        g_runs[i].pages -= n;
        if (g_runs[i].pages == 0) remove_run(i);
        g_free -= n * PAGE;
        memset(phys_to_virt(p), 0, n * PAGE);
        return p;
    }
    return 0;
}

void free_pages(uint64_t phys, size_t n) {
    if (phys && n) add_free(phys, n);
}

uint64_t total_bytes() { return g_total; }
uint64_t free_bytes() { return g_free; }

}  // namespace mm
