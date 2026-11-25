// SPDX-License-Identifier: GPL-2.0-only
#include "arch/x86_64/apic.h"
#include "acpi/acpi.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/idt.h"
#include "mm/vmm.h"

namespace apic {
namespace {

volatile uint32_t* g_lapic = nullptr;
volatile uint32_t* g_ioapic = nullptr;
uint32_t g_ioapic_gsi_base = 0;
uint32_t g_bsp_id = 0;
uint64_t g_tsc_hz = 0;
uint64_t g_tsc0 = 0;
const char* g_tsc_src = "none";
volatile uint64_t g_ticks = 0;
uint32_t g_hz = 0;
TickHook g_hook = nullptr;

uint32_t lr(uint32_t reg) { return g_lapic[reg / 4]; }
void lw(uint32_t reg, uint32_t v) { g_lapic[reg / 4] = v; }

uint32_t ior(uint32_t reg) {
    g_ioapic[0] = reg;
    return g_ioapic[4];
}
void iow(uint32_t reg, uint32_t v) {
    g_ioapic[0] = reg;
    g_ioapic[4] = v;
}

void disable_pic() {
    using namespace cpu;
    // Remap the 8259s away from the exception vectors, then mask everything.
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0xFF); outb(0xA1, 0xFF);
}

// PIT channel 2 one-shot wait used only as a calibration fallback (QEMU).
bool pit_calibrate_tsc() {
    using namespace cpu;
    const uint32_t PIT_HZ = 1193182, MS = 20;
    uint16_t count = uint16_t(PIT_HZ * MS / 1000);
    outb(0x61, uint8_t((inb(0x61) & ~0x02) | 0x01));  // gate on, speaker off
    outb(0x43, 0xB0);                               // ch2, lobyte/hibyte, mode 0
    outb(0x42, uint8_t(count));
    outb(0x42, uint8_t(count >> 8));
    uint64_t t0 = rdtsc();
    for (uint64_t spin = 0; !(inb(0x61) & 0x20); spin++)
        if (spin > 100000000ull) return false;  // PIT gated off
    uint64_t t1 = rdtsc();
    g_tsc_hz = (t1 - t0) * 1000 / MS;
    return g_tsc_hz > 0;
}

void calibrate_tsc() {
    uint32_t a, b, c, d;
    cpu::cpuid(0, 0, a, b, c, d);
    uint32_t max = a;
    if (max >= 0x15) {
        cpu::cpuid(0x15, 0, a, b, c, d);  // a = denominator, b = numerator, c = crystal Hz
        if (a && b && c) {
            g_tsc_hz = uint64_t(c) * b / a;
            g_tsc_src = "CPUID 15h";
            return;
        }
    }
    if (max >= 0x16) {
        cpu::cpuid(0x16, 0, a, b, c, d);  // a = base MHz
        if (a & 0xFFFF) {
            g_tsc_hz = uint64_t(a & 0xFFFF) * 1000000ull;
            g_tsc_src = "CPUID 16h";
            return;
        }
    }
    if (pit_calibrate_tsc()) {
        g_tsc_src = "PIT";
        return;
    }
    g_tsc_hz = 2000000000ull;  // last resort guess
    g_tsc_src = "guess 2GHz";
}

void timer_irq(idt::Frame*) {
    g_ticks = g_ticks + 1;
    eoi();
    if (g_hook) g_hook();
}

void spurious_irq(idt::Frame*) {}

}  // namespace

bool init() {
    const acpi::Info& ai = acpi::info();
    disable_pic();
    calibrate_tsc();
    g_tsc0 = cpu::rdtsc();
    g_lapic = static_cast<volatile uint32_t*>(vmm::map(ai.lapic_addr ? ai.lapic_addr : 0xFEE00000, 4096, vmm::Uncached));
    if (!g_lapic) return false;
    // Make sure the APIC is globally enabled (xAPIC mode is fine for the BSP).
    uint64_t base = cpu::rdmsr(0x1B);
    if (!(base & (1 << 11))) cpu::wrmsr(0x1B, base | (1 << 11));
    g_bsp_id = lr(0x20) >> 24;
    idt::set_handler(VEC_SPURIOUS, spurious_irq);
    idt::set_handler(VEC_TIMER, timer_irq);
    lw(0x80, 0);                          // TPR: accept all
    lw(0xF0, 0x100 | VEC_SPURIOUS);       // SVR: enable + spurious vector
    if (ai.n_ioapic > 0) {
        g_ioapic = static_cast<volatile uint32_t*>(vmm::map(ai.ioapic[0].addr, 4096, vmm::Uncached));
        g_ioapic_gsi_base = ai.ioapic[0].gsi_base;
    }
    return true;
}

void eoi() {
    if (g_lapic) lw(0xB0, 0);
}

bool route_isa_irq(uint8_t irq) {
    if (!g_ioapic) return false;
    const acpi::Info& ai = acpi::info();
    uint32_t gsi = irq;
    uint16_t flags = 0;
    for (int i = 0; i < ai.n_iso; i++)
        if (ai.iso[i].irq == irq) {
            gsi = ai.iso[i].gsi;
            flags = ai.iso[i].flags;
        }
    uint32_t maxred = (ior(1) >> 16) & 0xFF;
    if (gsi < g_ioapic_gsi_base || gsi - g_ioapic_gsi_base > maxred) return false;
    uint32_t pin = gsi - g_ioapic_gsi_base;
    uint32_t lo = VEC_IRQ_BASE + irq;           // fixed delivery, physical dest
    if ((flags & 3) == 3) lo |= 1 << 13;        // active low
    if (((flags >> 2) & 3) == 3) lo |= 1 << 15; // level triggered
    iow(0x10 + pin * 2 + 1, g_bsp_id << 24);
    iow(0x10 + pin * 2, lo);
    return true;
}

void start_timer(uint32_t hz) {
    if (!g_lapic || !hz) return;
    g_hz = hz;
    // Measure LAPIC timer frequency against the TSC over ~10 ms.
    lw(0x3E0, 0x3);          // divide by 16
    lw(0x320, 1 << 16);      // masked one-shot
    lw(0x380, 0xFFFFFFFF);
    uint64_t t0 = cpu::rdtsc(), wait = g_tsc_hz / 100;
    while (cpu::rdtsc() - t0 < wait) cpu::pause();
    uint32_t elapsed = 0xFFFFFFFF - lr(0x390);
    uint64_t lapic_hz = uint64_t(elapsed) * 100;
    lw(0x320, VEC_TIMER | (1 << 17));  // periodic
    lw(0x380, uint32_t(lapic_hz / hz ? lapic_hz / hz : 1));
}

uint64_t ticks() { return g_ticks; }
uint32_t timer_hz() { return g_hz; }
uint64_t tsc_hz() { return g_tsc_hz; }
const char* tsc_source() { return g_tsc_src; }
uint64_t uptime_ms() { return g_tsc_hz ? (cpu::rdtsc() - g_tsc0) / (g_tsc_hz / 1000) : 0; }

void sleep_ms(uint64_t ms) {
    uint64_t end = uptime_ms() + ms;
    while (uptime_ms() < end) cpu::sti_hlt();
}

void set_tick_hook(TickHook h) { g_hook = h; }

}  // namespace apic
