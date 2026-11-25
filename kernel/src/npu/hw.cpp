// SPDX-License-Identifier: GPL-2.0-only
#include "npu/hw.h"
#include <stdarg.h>
#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

namespace npu {
// Defined in npu.cpp: the log callback and the text for /sys/npu/bringup.txt.
void log_line(const char* line);
}

namespace npu::hw {
namespace {

struct Window {
    uint32_t base, size;
    volatile uint8_t* p;
};
// Every BAR0 range the driver touches (offsets from Linux ivpu_hw_ip_reg.h, ivpu_mmu.c).
Window g_win[] = {
    {0x00000000, 0x00050000, nullptr},  // HOST_SS, AON, ICB, IPC FIFO, LOADING_ADDRESS
    {0x00200000, 0x00020000, nullptr},  // NPU MMU (SMMUv3-like) + secure page
    {0x00360000, 0x00001000, nullptr},  // HOST_IF (TCU / TBU)
    {0x06010000, 0x00001000, nullptr},  // CPU_SS CPR (LEON_RT_VEC)
    {0x06020000, 0x00010000, nullptr},  // CPU_SS TIM (IPC FIFO to NPU, watchdog, free-running timer)
};
volatile uint8_t* g_regb = nullptr;
uint32_t g_bad_reported = 0;

volatile uint8_t* at(uint32_t off, uint32_t width) {
    for (Window& w : g_win)
        if (w.p && off >= w.base && off + width <= w.base + w.size) return w.p + (off - w.base);
    if (g_bad_reported++ < 4) out("    BUG: BAR0+0x%x is outside the mapped windows", off);
    return nullptr;
}

constexpr size_t MAX_BUFS = 512;  // buffers plus every NPU page-table page
struct Alloc {
    uint64_t phys;
    size_t pages;
};
Alloc g_allocs[MAX_BUFS];
size_t g_nallocs = 0, g_bytes = 0;

}  // namespace

void out(const char* fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log_line(line);
}

bool map_bars(uint64_t bar0, uint64_t bar4) {
    for (Window& w : g_win) {
        w.p = static_cast<volatile uint8_t*>(vmm::map(bar0 + w.base, w.size, vmm::Uncached));
        if (!w.p) return false;
    }
    g_regb = static_cast<volatile uint8_t*>(vmm::map(bar4, 0x1000, vmm::Uncached));
    return g_regb != nullptr;
}

uint32_t rv(uint32_t off) {
    volatile uint8_t* p = at(off, 4);
    return p ? *reinterpret_cast<volatile uint32_t*>(p) : 0xFFFFFFFFu;
}
void wv(uint32_t off, uint32_t v) {
    if (volatile uint8_t* p = at(off, 4)) *reinterpret_cast<volatile uint32_t*>(p) = v;
}
uint64_t rv64(uint32_t off) {
    volatile uint8_t* p = at(off, 8);
    return p ? *reinterpret_cast<volatile uint64_t*>(p) : ~0ull;
}
void wv64(uint32_t off, uint64_t v) {
    if (volatile uint8_t* p = at(off, 8)) *reinterpret_cast<volatile uint64_t*>(p) = v;
}
uint32_t rb(uint32_t off) { return *reinterpret_cast<volatile uint32_t*>(g_regb + off); }
void wb(uint32_t off, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(g_regb + off) = v; }

void udelay(uint64_t us) {
    uint64_t hz = apic::tsc_hz();
    if (!hz) hz = 3000000000ull;
    uint64_t end = cpu::rdtsc() + hz / 1000000 * us;
    while (cpu::rdtsc() < end) cpu::pause();
}

uint32_t fld(uint32_t v, uint32_t mask) { return (v & mask) >> __builtin_ctz(mask); }

bool poll(bool btrs, uint32_t reg, uint32_t mask, uint32_t want, uint64_t timeout_us, const char* what) {
    uint64_t waited = 0;
    for (;;) {
        uint32_t v = btrs ? rb(reg) : rv(reg);
        if (fld(v, mask) == want) return true;
        if (waited >= timeout_us) {
            out("    TIMEOUT: %s (%s+0x%x = 0x%08x, want %u)", what, btrs ? "BTRS" : "IP", reg, v, want);
            return false;
        }
        udelay(10);
        waited += 10;
    }
}

bool alloc(Buf& b, size_t size) {
    size_t pages = (size + mm::PAGE - 1) / mm::PAGE;
    if (g_nallocs == MAX_BUFS) return false;
    uint64_t phys = mm::alloc_pages(pages);
    if (!phys) return false;
    g_allocs[g_nallocs++] = {phys, pages};
    g_bytes += pages * mm::PAGE;
    b.phys = phys;
    b.cpu = mm::phys_to_virt<uint8_t>(phys);
    b.size = pages * mm::PAGE;
    b.vpu = 0;
    flush(b.cpu, b.size);  // the zeroes must reach memory, not stay in the cache
    return true;
}

void free_all() {
    for (size_t i = 0; i < g_nallocs; i++) mm::free_pages(g_allocs[i].phys, g_allocs[i].pages);
    g_nallocs = 0;
    g_bytes = 0;
}

size_t allocated_bytes() { return g_bytes; }

void flush(const void* p, size_t len) {
    uintptr_t a = uintptr_t(p) & ~uintptr_t(63), end = uintptr_t(p) + len;
    asm volatile("mfence" ::: "memory");
    for (; a < end; a += 64) asm volatile("clflush (%0)" ::"r"(a) : "memory");
    asm volatile("mfence" ::: "memory");
}

}  // namespace npu::hw
