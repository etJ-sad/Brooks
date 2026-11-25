// SPDX-License-Identifier: GPL-2.0-only
// Text console on top of the UEFI GOP framebuffer handed over by Limine.
// Keeps a character grid in RAM and repaints only dirty rows, so scrolling
// never reads back from (slow, write-combined) video memory.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct limine_framebuffer;

namespace con {

enum Color : uint8_t { Default, Dim, White, Green, Yellow, Red, Cyan, Magenta, Blue, Black, ColorCount };

struct Info {
    uint64_t width, height, pitch;
    uint16_t bpp;
    int font_w, font_h, scale;
    int cols, rows;
};

// font: "auto", "8x16" or "16x32"; scale: 0 = auto.
bool init(limine_framebuffer* fb, const char* font, int scale);
bool ready();
const Info& info();

void set_color(Color fg);
Color color();
void putc(char c);
void write(const char* s, size_t n);
void flush();

// Panes are rectangular scroll regions in character cells.
int add_pane(int x, int y, int w, int h);  // returns pane id, -1 on failure
void use_pane(int id);
int current_pane();
int cursor_row();  // absolute row of the current pane's cursor
void clear_pane();
void reset_full_screen();  // drop all panes, one full-screen pane (id 0)
void draw_vline(int col, int row0, int row1, Color c);

}  // namespace con
