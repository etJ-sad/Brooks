// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>

namespace cpu {

inline void outb(uint16_t port, uint8_t v) { asm volatile("outb %0, %1" ::"a"(v), "Nd"(port)); }
inline uint8_t inb(uint16_t port) {
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
inline void outw(uint16_t port, uint16_t v) { asm volatile("outw %0, %1" ::"a"(v), "Nd"(port)); }
inline void outl(uint16_t port, uint32_t v) { asm volatile("outl %0, %1" ::"a"(v), "Nd"(port)); }
inline uint32_t inl(uint16_t port) {
    uint32_t v;
    asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(sub));
}

inline uint64_t rdtsc() {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t(hi) << 32) | lo;
}

inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t(hi) << 32) | lo;
}
inline void wrmsr(uint32_t msr, uint64_t v) {
    asm volatile("wrmsr" ::"c"(msr), "a"(uint32_t(v)), "d"(uint32_t(v >> 32)));
}

inline void pause() { asm volatile("pause"); }
inline void sti() { asm volatile("sti"); }
inline void cli() { asm volatile("cli"); }
// Atomically enable interrupts and wait for the next one.
inline void sti_hlt() { asm volatile("sti; hlt"); }

inline uint64_t save_irq() {
    uint64_t f;
    asm volatile("pushfq; pop %0; cli" : "=r"(f)::"memory");
    return f;
}
inline void restore_irq(uint64_t f) {
    if (f & 0x200) asm volatile("sti" ::: "memory");
}

inline uint64_t read_cr2() {
    uint64_t v;
    asm volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}
inline uint64_t read_cr3() {
    uint64_t v;
    asm volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

[[noreturn]] inline void halt_forever() {
    for (;;) asm volatile("cli; hlt");
}

}  // namespace cpu
