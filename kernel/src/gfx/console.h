// SPDX-License-Identifier: GPL-2.0-only
// Text console on top of the UEFI GOP framebuffer handed over by Limine.
// Keeps a character grid in RAM and repaints only rows that changed, so
// scrolling never reads back from (slow, write-combined) video memory.
// Input is UTF-8; the font covers ASCII, Latin-1, Cyrillic and box drawing.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct limine_framebuffer;

namespace con {

enum Color : uint8_t {
    Default, Dim, White, Green, Yellow, Red, Cyan, Magenta, Blue, Black,
    // backgrounds / UI
    BgPanel, BgSelect, BgBar, BgDialog, BgInactiveSel,
    ColorCount
};

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

// ---- stream output (boot log)
void set_color(Color fg);
Color color();
void putc(char c);  // UTF-8 byte stream
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

// Everything printed so far (raw UTF-8), for /sys/bootlog.txt.
const char* log_data();
size_t log_size();

// ---- direct cell access for full-screen apps
// Draws UTF-8 text at (col,row), clipped to max_cols columns. Returns columns used.
int text(int col, int row, const char* s, Color fg, Color bg, int max_cols = 1 << 30);
void fill(int col, int row, int w, int h, uint32_t cp, Color fg, Color bg);
void cell(int col, int row, uint32_t cp, Color fg, Color bg);
void suspend_stream(bool on);  // while on, stream output goes only to serial + log

// UTF-8 helpers
uint32_t utf8_next(const char*& s);  // returns code point, advances s; 0 at end
int utf8_cols(const char* s);        // number of code points

}  // namespace con
