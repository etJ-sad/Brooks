// SPDX-License-Identifier: GPL-2.0-only
#pragma once

namespace serial {
// Initialise COM1 (0x3F8) at 115200 8N1. Returns false if no UART answers
// (typical on laptops); output is then silently dropped.
bool init();
bool present();
void putc(char c);
}  // namespace serial
