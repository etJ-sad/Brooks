// SPDX-License-Identifier: GPL-2.0-only
#include "gfx/console.h"
#include "drivers/serial.h"
#include "gfx/fonts/spleen16x32.h"
#include "gfx/fonts/spleen8x16.h"
#include "lib/string.h"
#include <limine.h>

namespace con {
namespace {

constexpr int MAX_COLS = 512;
constexpr int MAX_ROWS = 256;
constexpr int MAX_PANES = 8;

struct Cell {
    char ch;
    uint8_t color;
};

struct Pane {
    int x, y, w, h;
    int cx, cy;
};

Cell g_grid[MAX_ROWS][MAX_COLS];
bool g_dirty[MAX_ROWS];
Pane g_panes[MAX_PANES];
int g_npanes = 0;
int g_cur = 0;
Color g_fg = Default;
bool g_ready = false;

Info g_info;
uint8_t* g_fb = nullptr;
const uint8_t* g_font = nullptr;
int g_font_bpr = 1;
uint32_t g_rgb[ColorCount];
uint32_t g_bg_rgb;

// 0xRRGGBB palette, tuned for readability on a dark background.
constexpr uint32_t kPalette[ColorCount] = {
    0xC8CCD4,  // Default
    0x7A808C,  // Dim
    0xFFFFFF,  // White
    0x5FD75F,  // Green
    0xF0C850,  // Yellow
    0xFF5F5F,  // Red
    0x5FD7FF,  // Cyan
    0xD787FF,  // Magenta
    0x5F87FF,  // Blue
    0x101318,  // Black (background)
};

uint32_t to_native(uint32_t rgb, limine_framebuffer* fb) {
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    auto chan = [](uint32_t v, uint8_t size, uint8_t shift) -> uint32_t {
        if (size < 8) v >>= (8 - size);
        return v << shift;
    };
    return chan(r, fb->red_mask_size, fb->red_mask_shift) | chan(g, fb->green_mask_size, fb->green_mask_shift) |
           chan(b, fb->blue_mask_size, fb->blue_mask_shift);
}

inline void put_px(uint8_t* row, uint64_t x, uint32_t v) {
    if (g_info.bpp == 32) {
        reinterpret_cast<volatile uint32_t*>(row)[x] = v;
    } else {  // 24 bpp
        volatile uint8_t* p = row + x * 3;
        p[0] = uint8_t(v);
        p[1] = uint8_t(v >> 8);
        p[2] = uint8_t(v >> 16);
    }
}

void draw_cell(int col, int row) {
    const Cell& c = g_grid[row][col];
    const int fw = g_info.font_w, fh = g_info.font_h, s = g_info.scale;
    const uint8_t* glyph = g_font + size_t(uint8_t(c.ch)) * size_t(g_font_bpr * fh);
    const uint32_t fg = g_rgb[c.color < ColorCount ? c.color : 0];
    const uint64_t px0 = uint64_t(col) * fw * s;
    const uint64_t py0 = uint64_t(row) * fh * s;
    for (int gy = 0; gy < fh; gy++) {
        const uint8_t* bits = glyph + gy * g_font_bpr;
        for (int sy = 0; sy < s; sy++) {
            uint8_t* line = g_fb + (py0 + uint64_t(gy) * s + sy) * g_info.pitch;
            for (int gx = 0; gx < fw; gx++) {
                bool on = c.ch != ' ' && ((bits[gx >> 3] >> (7 - (gx & 7))) & 1);
                uint32_t v = on ? fg : g_bg_rgb;
                for (int sx = 0; sx < s; sx++) put_px(line, px0 + uint64_t(gx) * s + sx, v);
            }
        }
    }
}

void clear_rows(int x, int y, int w, int h) {
    for (int r = y; r < y + h; r++) {
        for (int c = x; c < x + w; c++) g_grid[r][c] = {' ', Default};
        g_dirty[r] = true;
    }
}

void scroll(Pane& p) {
    for (int r = p.y; r < p.y + p.h - 1; r++) {
        memcpy(&g_grid[r][p.x], &g_grid[r + 1][p.x], sizeof(Cell) * p.w);
        g_dirty[r] = true;
    }
    clear_rows(p.x, p.y + p.h - 1, p.w, 1);
}

void newline(Pane& p) {
    p.cx = 0;
    if (++p.cy >= p.h) {
        scroll(p);
        p.cy = p.h - 1;
    }
}

}  // namespace

bool init(limine_framebuffer* fb, const char* font, int scale) {
    if (!fb || (fb->bpp != 32 && fb->bpp != 24) || fb->memory_model != LIMINE_FRAMEBUFFER_RGB) return false;
    g_fb = static_cast<uint8_t*>(fb->address);
    g_info.width = fb->width;
    g_info.height = fb->height;
    g_info.pitch = fb->pitch;
    g_info.bpp = fb->bpp;

    bool big;
    if (font && memcmp(font, "8x16", 5) == 0) big = false;
    else if (font && memcmp(font, "16x32", 6) == 0) big = true;
    else big = fb->height >= 1440;  // 4K / 1600p panels get the 16x32 font
    if (big) {
        g_font = &font::spleen16x32_data[0][0];
        g_font_bpr = font::spleen16x32_bpr;
        g_info.font_w = font::spleen16x32_w;
        g_info.font_h = font::spleen16x32_h;
    } else {
        g_font = &font::spleen8x16_data[0][0];
        g_font_bpr = font::spleen8x16_bpr;
        g_info.font_w = font::spleen8x16_w;
        g_info.font_h = font::spleen8x16_h;
    }
    if (scale <= 0) scale = 1;
    while (scale > 1 && (fb->width / (g_info.font_w * scale) < 80 || fb->height / (g_info.font_h * scale) < 25)) scale--;
    g_info.scale = scale;
    g_info.cols = int(fb->width / (g_info.font_w * scale));
    g_info.rows = int(fb->height / (g_info.font_h * scale));
    if (g_info.cols > MAX_COLS) g_info.cols = MAX_COLS;
    if (g_info.rows > MAX_ROWS) g_info.rows = MAX_ROWS;

    for (int i = 0; i < ColorCount; i++) g_rgb[i] = to_native(kPalette[i], fb);
    g_bg_rgb = g_rgb[Black];

    // Paint the whole framebuffer (including the margin not covered by cells).
    for (uint64_t y = 0; y < fb->height; y++) {
        uint8_t* line = g_fb + y * fb->pitch;
        for (uint64_t x = 0; x < fb->width; x++) put_px(line, x, g_bg_rgb);
    }
    g_ready = true;
    reset_full_screen();
    return true;
}

bool ready() { return g_ready; }
const Info& info() { return g_info; }
void set_color(Color fg) { g_fg = fg; }
Color color() { return g_fg; }

void putc(char c) {
    serial::putc(c);
    if (!g_ready) return;
    Pane& p = g_panes[g_cur];
    if (c == '\n') {
        newline(p);
        flush();
        return;
    }
    if (c == '\r') { p.cx = 0; return; }
    if (c == '\t') {
        do putc(' '); while (p.cx % 8);
        return;
    }
    if (p.cx >= p.w) newline(p);
    int r = p.y + p.cy, col = p.x + p.cx;
    g_grid[r][col] = {c, uint8_t(g_fg)};
    g_dirty[r] = true;
    p.cx++;
}

void write(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) putc(s[i]);
}

void flush() {
    if (!g_ready) return;
    for (int r = 0; r < g_info.rows; r++) {
        if (!g_dirty[r]) continue;
        for (int c = 0; c < g_info.cols; c++) draw_cell(c, r);
        g_dirty[r] = false;
    }
}

int add_pane(int x, int y, int w, int h) {
    if (g_npanes >= MAX_PANES || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > g_info.cols || y + h > g_info.rows)
        return -1;
    g_panes[g_npanes] = {x, y, w, h, 0, 0};
    return g_npanes++;
}

void use_pane(int id) {
    if (id >= 0 && id < g_npanes) g_cur = id;
}
int current_pane() { return g_cur; }
int cursor_row() { return g_ready ? g_panes[g_cur].y + g_panes[g_cur].cy : 0; }

void clear_pane() {
    if (!g_ready) return;
    Pane& p = g_panes[g_cur];
    clear_rows(p.x, p.y, p.w, p.h);
    p.cx = p.cy = 0;
    flush();
}

void reset_full_screen() {
    if (!g_ready) return;
    g_npanes = 0;
    add_pane(0, 0, g_info.cols, g_info.rows);
    g_cur = 0;
    clear_rows(0, 0, g_info.cols, g_info.rows);
    flush();
}

void draw_vline(int col, int row0, int row1, Color c) {
    if (!g_ready || col < 0 || col >= g_info.cols) return;
    for (int r = row0; r <= row1 && r < g_info.rows; r++) {
        g_grid[r][col] = {'|', uint8_t(c)};
        g_dirty[r] = true;
    }
    flush();
}

}  // namespace con
