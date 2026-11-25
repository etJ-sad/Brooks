// SPDX-License-Identifier: GPL-2.0-only
// Local APIC + IOAPIC interrupt routing, TSC calibration, periodic timer.
#pragma once
#include <stdint.h>

namespace apic {

constexpr uint8_t VEC_TIMER = 0x30;
constexpr uint8_t VEC_IRQ_BASE = 0x40;  // legacy ISA IRQ n -> vector 0x40 + n
constexpr uint8_t VEC_SPURIOUS = 0xFF;

bool init();  // needs acpi::init() first
void eoi();
// Route legacy ISA IRQ (0..15) to VEC_IRQ_BASE + irq on the BSP.
bool route_isa_irq(uint8_t irq);

void start_timer(uint32_t hz);
uint64_t ticks();
uint32_t timer_hz();
uint64_t tsc_hz();
uint64_t uptime_ms();
void sleep_ms(uint64_t ms);
const char* tsc_source();

using TickHook = void (*)();
void set_tick_hook(TickHook h);

}  // namespace apic
