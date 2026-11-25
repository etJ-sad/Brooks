// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>

namespace idt {

struct Frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};

using Handler = void (*)(Frame*);
// Handler for vectors 32..255 (device interrupts). The handler sends EOI itself.
void set_handler(uint8_t vector, Handler h);

// Install handlers for CPU exceptions 0..31. Any exception shows a panic
// screen with registers instead of a silent triple-fault reboot.
void init();

}  // namespace idt

[[noreturn]] void panic(const char* msg);
