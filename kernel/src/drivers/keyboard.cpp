// SPDX-License-Identifier: GPL-2.0-only
#include "drivers/keyboard.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/idt.h"

namespace kbd {
namespace {

constexpr uint16_t DATA = 0x60, STATUS = 0x64, CMD = 0x64;
bool g_present = false;
volatile uint32_t g_irqs = 0;

Event g_q[128];
volatile uint32_t g_head = 0, g_tail = 0;

bool g_e0 = false;
bool g_lshift = false, g_rshift = false, g_ctrl = false, g_alt = false, g_caps = false;

// Scancode set 1 (translated) -> US layout.
const char kNormal[0x3A] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
};
const char kShift[0x3A] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
};

void push(Key k, char ch = 0) {
    uint32_t next = (g_head + 1) % 128;
    if (next == g_tail) return;  // full: drop
    uint8_t m = uint8_t(((g_lshift || g_rshift) ? Shift : 0) | (g_ctrl ? Ctrl : 0) | (g_alt ? Alt : 0));
    g_q[g_head] = {k, ch, m};
    g_head = next;
}

void handle(uint8_t sc) {
    if (sc == 0xE0) {
        g_e0 = true;
        return;
    }
    if (sc == 0xE1) return;  // Pause: ignore
    bool release = sc & 0x80;
    uint8_t code = sc & 0x7F;
    bool e0 = g_e0;
    g_e0 = false;

    switch (code) {
    case 0x2A: if (!e0) g_lshift = !release; return;  // E0 2A is fake shift
    case 0x36: if (!e0) g_rshift = !release; return;
    case 0x1D: g_ctrl = !release; return;
    case 0x38: g_alt = !release; return;
    }
    if (release) return;

    if (e0) {
        switch (code) {
        case 0x48: push(Up); return;
        case 0x50: push(Down); return;
        case 0x4B: push(Left); return;
        case 0x4D: push(Right); return;
        case 0x47: push(Home); return;
        case 0x4F: push(End); return;
        case 0x49: push(PageUp); return;
        case 0x51: push(PageDown); return;
        case 0x52: push(Insert); return;
        case 0x53: push(Delete); return;
        case 0x1C: push(Enter); return;      // keypad Enter
        case 0x35: push(Char, '/'); return;  // keypad /
        default: return;
        }
    }
    switch (code) {
    case 0x01: push(Esc); return;
    case 0x0E: push(Backspace); return;
    case 0x0F: push(Tab); return;
    case 0x1C: push(Enter); return;
    case 0x3A: g_caps = !g_caps; return;
    case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44:
        push(Key(F1 + (code - 0x3B)));
        return;
    case 0x57: push(F11); return;
    case 0x58: push(F12); return;
    // Keypad without NumLock handling: treat as navigation keys.
    case 0x48: push(Up); return;
    case 0x50: push(Down); return;
    case 0x4B: push(Left); return;
    case 0x4D: push(Right); return;
    case 0x47: push(Home); return;
    case 0x4F: push(End); return;
    case 0x49: push(PageUp); return;
    case 0x51: push(PageDown); return;
    case 0x53: push(Delete); return;
    }
    if (code < 0x3A) {
        bool shift = g_lshift || g_rshift;
        char c = shift ? kShift[code] : kNormal[code];
        if (g_caps && c >= 'a' && c <= 'z') c = char(c - 32);
        else if (g_caps && c >= 'A' && c <= 'Z') c = char(c + 32);
        if (c) push(Char, c);
    }
}

void irq(idt::Frame*) {
    g_irqs = g_irqs + 1;
    service();
    apic::eoi();
}

bool wait_write() {
    for (int i = 0; i < 100000; i++)
        if (!(cpu::inb(STATUS) & 2)) return true;
    return false;
}
bool wait_read() {
    for (int i = 0; i < 100000; i++)
        if (cpu::inb(STATUS) & 1) return true;
    return false;
}

}  // namespace

void service() {
    if (!g_present) return;
    for (int n = 0; n < 32; n++) {
        uint8_t st = cpu::inb(STATUS);
        if (!(st & 1)) break;
        uint8_t b = cpu::inb(DATA);
        if (st & 0x20) continue;  // mouse byte
        handle(b);
    }
}

bool init() {
    if (cpu::inb(STATUS) == 0xFF) return false;
    g_present = true;
    // Flush, enable IRQ1 + translation on port 1, keep port 2 as is.
    for (int i = 0; i < 64 && (cpu::inb(STATUS) & 1); i++) cpu::inb(DATA);
    if (wait_write()) cpu::outb(CMD, 0x20);
    uint8_t cfg = wait_read() ? cpu::inb(DATA) : 0x45;
    cfg |= 0x01 | 0x40;  // IRQ1, translation to set 1
    cfg &= uint8_t(~0x10);  // port 1 clock enabled
    if (wait_write()) cpu::outb(CMD, 0x60);
    if (wait_write()) cpu::outb(DATA, cfg);
    if (wait_write()) cpu::outb(CMD, 0xAE);  // enable port 1
    if (wait_write()) cpu::outb(DATA, 0xF4);  // enable scanning
    if (wait_read()) cpu::inb(DATA);          // ACK (0xFA)
    idt::set_handler(apic::VEC_IRQ_BASE + 1, irq);
    apic::route_isa_irq(1);
    return true;
}

bool present() { return g_present; }
uint32_t irq_count() { return g_irqs; }

bool poll(Event& out) {
    uint64_t f = cpu::save_irq();
    bool have = g_tail != g_head;
    if (have) {
        out = g_q[g_tail];
        g_tail = (g_tail + 1) % 128;
    }
    cpu::restore_irq(f);
    return have;
}

Event wait() {
    Event e;
    for (;;) {
        cpu::cli();
        if (poll(e)) {
            cpu::sti();
            return e;
        }
        cpu::sti_hlt();  // atomically enable IRQs and sleep
    }
}

}  // namespace kbd
