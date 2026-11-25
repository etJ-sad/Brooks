// SPDX-License-Identifier: GPL-2.0-only
// PS/2 (i8042) keyboard. On most laptops the built-in keyboard sits behind the
// embedded controller's i8042 emulation; USB keyboards work only if the
// firmware's legacy USB emulation is still active.
#pragma once
#include <stdint.h>

namespace kbd {

enum Key : uint16_t {
    None = 0,
    Char,  // printable, see Event::ch
    Enter, Esc, Backspace, Tab,
    Up, Down, Left, Right, Home, End, PageUp, PageDown, Insert, Delete,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

enum Mod : uint8_t { Shift = 1, Ctrl = 2, Alt = 4 };

struct Event {
    Key key;
    char ch;
    uint8_t mods;
};

bool init();  // returns false if no controller answers
bool present();
bool poll(Event& out);   // non-blocking
Event wait();            // sleeps with hlt until a key arrives
void service();          // drain the controller (called from timer as fallback)
uint32_t irq_count();

}  // namespace kbd
