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
