// SPDX-License-Identifier: GPL-2.0-only
// Small text-mode UI toolkit shared by the file manager, viewer and editor.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "drivers/keyboard.h"
#include "gfx/console.h"

namespace ui {

int cols();
int rows();

void box(int x, int y, int w, int h, con::Color fg, con::Color bg, const char* title = nullptr, bool active = true);
// Bottom key bar: "1Help 2Log ..." pairs of (number, label).
void keybar(const char* const labels[10]);
void topbar(const char* left);

// Modal dialogs. Return false on Esc.
bool input(const char* title, const char* prompt, char* buf, size_t cap);
bool confirm(const char* title, const char* question);
void message(const char* title, const char* text);
// Returns the pressed key's char (lower-case) among `keys`, or 0 on Esc.
char choose(const char* title, const char* text, const char* keys);

// Next key; while waiting, calls idle() roughly once a second (clock refresh).
kbd::Event next_key(void (*idle)() = nullptr);

void fmt_size(uint64_t bytes, char* out, size_t cap);
void clock_str(char* out, size_t cap);

}  // namespace ui
