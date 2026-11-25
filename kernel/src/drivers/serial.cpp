// SPDX-License-Identifier: GPL-2.0-only
#include "drivers/serial.h"
#include "arch/x86_64/cpu.h"

namespace serial {
namespace {
constexpr uint16_t COM1 = 0x3F8;
bool g_ok = false;
}

bool init() {
    using namespace cpu;
    outb(COM1 + 1, 0x00);  // no interrupts
    outb(COM1 + 3, 0x80);  // DLAB
    outb(COM1 + 0, 0x01);  // divisor 1 -> 115200
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);  // 8N1
    outb(COM1 + 2, 0xC7);  // FIFO
    outb(COM1 + 4, 0x1E);  // loopback test
    outb(COM1 + 0, 0xAE);
    if (inb(COM1 + 0) != 0xAE) { g_ok = false; return false; }
    outb(COM1 + 4, 0x0F);  // normal mode
    g_ok = true;
    return true;
}

bool present() { return g_ok; }

void putc(char c) {
    if (!g_ok) return;
    if (c == '\n') putc('\r');
    for (int spin = 0; spin < 100000 && !(cpu::inb(COM1 + 5) & 0x20); spin++) {}
    cpu::outb(COM1, uint8_t(c));
}

}  // namespace serial
