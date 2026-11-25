// SPDX-License-Identifier: GPL-2.0-only
#include "drivers/rtc.h"
#include "arch/x86_64/cpu.h"

namespace rtc {
namespace {
uint8_t reg(uint8_t r) {
    cpu::outb(0x70, uint8_t(0x80 | r));  // keep NMI disabled bit set while reading
    return cpu::inb(0x71);
}
uint8_t bcd(uint8_t v) { return uint8_t((v & 0x0F) + (v >> 4) * 10); }
}  // namespace

bool read(Time& t) {
    for (int i = 0; i < 100000 && (reg(0x0A) & 0x80); i++) cpu::pause();  // update in progress
    uint8_t s = reg(0x00), m = reg(0x02), h = reg(0x04), d = reg(0x07), mo = reg(0x08), y = reg(0x09);
    uint8_t b = reg(0x0B);
    if (s == 0xFF && m == 0xFF) return false;
    bool pm = h & 0x80;
    h &= 0x7F;
    if (!(b & 0x04)) {  // BCD mode
        s = bcd(s); m = bcd(m); h = bcd(h); d = bcd(d); mo = bcd(mo); y = bcd(y);
    }
    if (!(b & 0x02) && pm) h = uint8_t((h % 12) + 12);  // 12-hour mode
    t = {uint16_t(2000 + y), mo, d, h, m, s};
    return true;
}
}  // namespace rtc
