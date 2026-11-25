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
constexpr size_t LOG_SIZE = 256 * 1024;

struct Cell {
    uint16_t g;  // glyph index
    uint8_t fg, bg;
    bool operator==(const Cell& o) const { return g == o.g && fg == o.fg && bg == o.bg; }
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
bool g_suspended = false;

char g_log[LOG_SIZE];
size_t g_log_len = 0;

// UTF-8 stream decoder state
uint32_t g_u8_cp = 0;
int g_u8_left = 0;

Info g_info;
uint8_t* g_fb = nullptr;
const uint8_t* g_font = nullptr;
const uint16_t* g_cps = nullptr;
int g_nglyphs = 0;
int g_glyph_bytes = 0;
int g_font_bpr = 1;
uint32_t g_rgb[ColorCount];
uint16_t g_space = 0;

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
    0x141C2B,  // BgPanel
    0x2E6DB4,  // BgSelect
    0x262B36,  // BgBar
    0x3A3F4E,  // BgDialog
    0x2A3550,  // BgInactiveSel
};

uint16_t glyph_for(uint32_t cp) {
    int lo = 1, hi = g_nglyphs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_cps[mid] == cp) return uint16_t(mid);
        if (g_cps[mid] < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;  // '?'
}

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
    const uint8_t* glyph = g_font + size_t(c.g < g_nglyphs ? c.g : 0) * size_t(g_glyph_bytes);
    const uint32_t fg = g_rgb[c.fg < ColorCount ? c.fg : 0];
    const uint32_t bg = g_rgb[c.bg < ColorCount ? c.bg : Black];
    const uint64_t px0 = uint64_t(col) * fw * s;
    const uint64_t py0 = uint64_t(row) * fh * s;
    for (int gy = 0; gy < fh; gy++) {
        const uint8_t* bits = glyph + gy * g_font_bpr;
        for (int sy = 0; sy < s; sy++) {
            uint8_t* line = g_fb + (py0 + uint64_t(gy) * s + sy) * g_info.pitch;
            for (int gx = 0; gx < fw; gx++) {
                bool on = (bits[gx >> 3] >> (7 - (gx & 7))) & 1;
                uint32_t v = on ? fg : bg;
                for (int sx = 0; sx < s; sx++) put_px(line, px0 + uint64_t(gx) * s + sx, v);
            }
        }
    }
}

inline void set(int col, int row, Cell c) {
    if (row < 0 || row >= g_info.rows || col < 0 || col >= g_info.cols) return;
    if (g_grid[row][col] == c) return;
    g_grid[row][col] = c;
    g_dirty[row] = true;
}

void clear_rows(int x, int y, int w, int h) {
    for (int r = y; r < y + h; r++)
        for (int c = x; c < x + w; c++) set(c, r, {g_space, Default, Black});
}

void scroll(Pane& p) {
    for (int r = p.y; r < p.y + p.h - 1; r++)
        for (int c = p.x; c < p.x + p.w; c++) set(c, r, g_grid[r + 1][c]);
    clear_rows(p.x, p.y + p.h - 1, p.w, 1);
}

void newline(Pane& p) {
    p.cx = 0;
    if (++p.cy >= p.h) {
        scroll(p);
        p.cy = p.h - 1;
    }
}

void put_cp(uint32_t cp) {
    Pane& p = g_panes[g_cur];
    if (cp == '\n') {
        newline(p);
        flush();
        return;
    }
    if (cp == '\r') { p.cx = 0; return; }
    if (cp == '\t') {
        do put_cp(' '); while (p.cx % 8);
        return;
    }
    if (p.cx >= p.w) newline(p);
    set(p.x + p.cx, p.y + p.cy, {glyph_for(cp), uint8_t(g_fg), Black});
    p.cx++;
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
        g_cps = font::spleen16x32_cp;
        g_nglyphs = font::spleen16x32_count;
        g_font_bpr = font::spleen16x32_bpr;
        g_info.font_w = font::spleen16x32_w;
        g_info.font_h = font::spleen16x32_h;
    } else {
        g_font = &font::spleen8x16_data[0][0];
        g_cps = font::spleen8x16_cp;
        g_nglyphs = font::spleen8x16_count;
        g_font_bpr = font::spleen8x16_bpr;
        g_info.font_w = font::spleen8x16_w;
        g_info.font_h = font::spleen8x16_h;
    }
    g_glyph_bytes = g_font_bpr * g_info.font_h;
    g_space = glyph_for(' ');
    if (scale <= 0) scale = 1;
    while (scale > 1 && (fb->width / (g_info.font_w * scale) < 80 || fb->height / (g_info.font_h * scale) < 25)) scale--;
    g_info.scale = scale;
    g_info.cols = int(fb->width / (g_info.font_w * scale));
    g_info.rows = int(fb->height / (g_info.font_h * scale));
    if (g_info.cols > MAX_COLS) g_info.cols = MAX_COLS;
    if (g_info.rows > MAX_ROWS) g_info.rows = MAX_ROWS;

    for (int i = 0; i < ColorCount; i++) g_rgb[i] = to_native(kPalette[i], fb);

    // Paint the whole framebuffer (including the margin not covered by cells).
    for (uint64_t y = 0; y < fb->height; y++) {
        uint8_t* line = g_fb + y * fb->pitch;
        for (uint64_t x = 0; x < fb->width; x++) put_px(line, x, g_rgb[Black]);
    }
    for (int r = 0; r < g_info.rows; r++)
        for (int c = 0; c < g_info.cols; c++) g_grid[r][c] = {g_space, Default, Black};
    g_ready = true;
    reset_full_screen();
    return true;
}

bool ready() { return g_ready; }
const Info& info() { return g_info; }
void set_color(Color fg) { g_fg = fg; }
Color color() { return g_fg; }
void suspend_stream(bool on) { g_suspended = on; }

void putc(char ch) {
    serial::putc(ch);
    if (g_log_len < LOG_SIZE) g_log[g_log_len++] = ch;
    if (!g_ready || g_suspended) return;
    uint8_t b = uint8_t(ch);
    if (g_u8_left > 0 && (b & 0xC0) == 0x80) {
        g_u8_cp = (g_u8_cp << 6) | (b & 0x3F);
        if (--g_u8_left == 0) put_cp(g_u8_cp);
        return;
    }
    g_u8_left = 0;
    if (b < 0x80) put_cp(b);
    else if ((b & 0xE0) == 0xC0) { g_u8_cp = b & 0x1F; g_u8_left = 1; }
    else if ((b & 0xF0) == 0xE0) { g_u8_cp = b & 0x0F; g_u8_left = 2; }
    else if ((b & 0xF8) == 0xF0) { g_u8_cp = b & 0x07; g_u8_left = 3; }
    else put_cp('?');
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
    g_suspended = false;
    clear_rows(0, 0, g_info.cols, g_info.rows);
    flush();
}

void draw_vline(int col, int row0, int row1, Color c) {
    if (!g_ready) return;
    for (int r = row0; r <= row1; r++) set(col, r, {glyph_for(0x2502), uint8_t(c), Black});
    flush();
}

const char* log_data() { return g_log; }
size_t log_size() { return g_log_len; }

uint32_t utf8_next(const char*& s) {
    uint8_t b = uint8_t(*s);
    if (!b) return 0;
    s++;
    if (b < 0x80) return b;
    int extra = (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : 0;
    uint32_t cp = extra == 1 ? (b & 0x1F) : extra == 2 ? (b & 0x0F) : (b & 0x07);
    if (!extra) return '?';
    for (int i = 0; i < extra; i++) {
        if ((uint8_t(*s) & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | (uint8_t(*s++) & 0x3F);
    }
    return cp;
}

int utf8_cols(const char* s) {
    int n = 0;
    while (utf8_next(s)) n++;
    return n;
}

int text(int col, int row, const char* s, Color fg, Color bg, int max_cols) {
    int n = 0;
    uint32_t cp;
    while (n < max_cols && (cp = utf8_next(s)) != 0) {
        if (cp < 0x20) cp = ' ';
        set(col + n, row, {glyph_for(cp), uint8_t(fg), uint8_t(bg)});
        n++;
    }
    return n;
}

void fill(int col, int row, int w, int h, uint32_t cp, Color fg, Color bg) {
    uint16_t g = glyph_for(cp);
    for (int r = row; r < row + h; r++)
        for (int c = col; c < col + w; c++) set(c, r, {g, uint8_t(fg), uint8_t(bg)});
}

void cell(int col, int row, uint32_t cp, Color fg, Color bg) { set(col, row, {glyph_for(cp), uint8_t(fg), uint8_t(bg)}); }

}  // namespace con
