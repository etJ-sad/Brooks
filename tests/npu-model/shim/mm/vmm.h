#pragma once
#include <stdint.h>
#include <stddef.h>
extern uint8_t* g_bar0; extern volatile uint32_t g_b[0x400];
namespace vmm { enum Cache { WriteBack, Uncached };
inline void* map(uint64_t phys, size_t, Cache) {
    if (phys >= 0x10000000 && phys < 0x20000000) return g_bar0 + (phys - 0x10000000);
    if (phys == 0x2000) return (void*)g_b; return nullptr; } }
