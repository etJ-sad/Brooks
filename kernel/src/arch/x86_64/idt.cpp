// SPDX-License-Identifier: GPL-2.0-only
#include "arch/x86_64/idt.h"
#include "arch/x86_64/cpu.h"
#include "lib/kprintf.h"

extern "C" const uint64_t isr_stub_table[32];

namespace idt {
namespace {

struct [[gnu::packed]] Gate {
    uint16_t off_lo, sel;
    uint8_t ist, attr;
    uint16_t off_mid;
    uint32_t off_hi, zero;
};

struct [[gnu::packed]] Idtr {
    uint16_t limit;
    uint64_t base;
};

Gate g_idt[256];

const char* const kNames[32] = {
    "#DE divide error", "#DB debug", "NMI", "#BP breakpoint", "#OF overflow", "#BR bound range",
    "#UD invalid opcode", "#NM device not available", "#DF double fault", "coprocessor overrun",
    "#TS invalid TSS", "#NP segment not present", "#SS stack fault", "#GP general protection",
    "#PF page fault", "reserved", "#MF x87 FP", "#AC alignment check", "#MC machine check",
    "#XM SIMD FP", "#VE virtualization", "#CP control protection", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved", "#HV hypervisor injection", "#VC VMM communication",
    "#SX security", "reserved",
};

}  // namespace

void init() {
    uint16_t cs;
    asm volatile("mov %%cs, %0" : "=r"(cs));
    for (int i = 0; i < 32; i++) {
        uint64_t a = isr_stub_table[i];
        g_idt[i] = {uint16_t(a), cs, 0, 0x8E, uint16_t(a >> 16), uint32_t(a >> 32), 0};
    }
    Idtr r{sizeof(g_idt) - 1, uint64_t(&g_idt)};
    asm volatile("lidt %0" ::"m"(r));
}

}  // namespace idt

static void panic_header() {
    con::reset_full_screen();
    con::set_color(con::Red);
    kprintf("\n  *** KERNEL PANIC ***\n\n");
}

extern "C" [[noreturn]] void isr_dispatch(idt::Frame* f) {
    panic_header();
    con::set_color(con::White);
    kprintf("  Exception %lu: %s   error=0x%lx\n\n", f->vector, f->vector < 32 ? idt::kNames[f->vector] : "?", f->error);
    con::set_color(con::Default);
    kprintf("  RIP=%016lx  CS=%04lx  RFLAGS=%016lx\n", f->rip, f->cs, f->rflags);
    kprintf("  RSP=%016lx  SS=%04lx  CR2=%016lx  CR3=%016lx\n\n", f->rsp, f->ss, cpu::read_cr2(), cpu::read_cr3());
    kprintf("  RAX=%016lx RBX=%016lx RCX=%016lx RDX=%016lx\n", f->rax, f->rbx, f->rcx, f->rdx);
    kprintf("  RSI=%016lx RDI=%016lx RBP=%016lx\n", f->rsi, f->rdi, f->rbp);
    kprintf("  R8 =%016lx R9 =%016lx R10=%016lx R11=%016lx\n", f->r8, f->r9, f->r10, f->r11);
    kprintf("  R12=%016lx R13=%016lx R14=%016lx R15=%016lx\n\n", f->r12, f->r13, f->r14, f->r15);
    kcprintf(con::Yellow, "  Photograph this screen. Power off with the power button.\n");
    cpu::halt_forever();
}

void panic(const char* msg) {
    panic_header();
    kcprintf(con::White, "  %s\n", msg);
    cpu::halt_forever();
}
