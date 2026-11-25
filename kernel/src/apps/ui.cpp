// SPDX-License-Identifier: GPL-2.0-only
#include "apps/ui.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "drivers/rtc.h"
#include "lib/kprintf.h"
#include "lib/string.h"

namespace ui {

using namespace con;

int cols() { return info().cols; }
int rows() { return info().rows; }

void box(int x, int y, int w, int h, Color fg, Color bg, const char* title, bool active) {
    fill(x, y, w, h, ' ', fg, bg);
    Color bc = active ? fg : Dim;
    cell(x, y, 0x250C, bc, bg);
    cell(x + w - 1, y, 0x2510, bc, bg);
    cell(x, y + h - 1, 0x2514, bc, bg);
    cell(x + w - 1, y + h - 1, 0x2518, bc, bg);
    fill(x + 1, y, w - 2, 1, 0x2500, bc, bg);
    fill(x + 1, y + h - 1, w - 2, 1, 0x2500, bc, bg);
    fill(x, y + 1, 1, h - 2, 0x2502, bc, bg);
    fill(x + w - 1, y + 1, 1, h - 2, 0x2502, bc, bg);
    if (title && *title) {
        int tl = utf8_cols(title);
        int max = w - 4;
        if (tl > max) tl = max;
        int tx = x + (w - tl - 2) / 2;
        cell(tx, y, ' ', bc, bg);
        text(tx + 1, y, title, active ? Black : Default, active ? Cyan : BgBar, max);
        cell(tx + 1 + tl, y, ' ', bc, bg);
    }
}

void keybar(const char* const labels[10]) {
    int w = cols(), y = rows() - 1;
    fill(0, y, w, 1, ' ', Default, Black);
    int slot = w / 10;
    for (int i = 0; i < 10; i++) {
        int x = i * slot;
        char num[4];
        ksnprintf(num, sizeof(num), "%d", i + 1 == 10 ? 10 : i + 1);
        int n = text(x, y, num, White, Black);
        fill(x + n, y, slot - n, 1, ' ', Black, Cyan);
        if (labels[i]) text(x + n, y, labels[i], Black, Cyan, slot - n - 1);
    }
}

void clock_str(char* out, size_t cap) {
    rtc::Time t;
    uint64_t up = apic::uptime_ms() / 1000;
    if (rtc::read(t))
        ksnprintf(out, cap, "%02u:%02u:%02u  работает %lu:%02lu", t.hour, t.min, t.sec, up / 60, up % 60);
    else
        ksnprintf(out, cap, "работает %lu:%02lu", up / 60, up % 60);
}

void topbar(const char* left) {
    int w = cols();
    fill(0, 0, w, 1, ' ', White, BgBar);
    text(1, 0, left, White, BgBar, w - 2);
}

void fmt_size(uint64_t b, char* out, size_t cap) {
    if (b < 10000) ksnprintf(out, cap, "%lu B", b);
    else if (b < 10000ull * 1024) ksnprintf(out, cap, "%lu K", (b + 1023) / 1024);
    else if (b < 10000ull * 1024 * 1024) ksnprintf(out, cap, "%lu M", (b + (1 << 20) - 1) >> 20);
    else ksnprintf(out, cap, "%lu G", uint64_t((b + (uint64_t(1) << 30) - 1) >> 30));
}

kbd::Event next_key(void (*idle)()) {
    uint64_t last = apic::uptime_ms() / 1000;
    for (;;) {
        kbd::Event e;
        cpu::cli();
        if (kbd::poll(e)) {
            cpu::sti();
            return e;
        }
        cpu::sti_hlt();
        uint64_t now = apic::uptime_ms() / 1000;
        if (now != last) {
            last = now;
            if (idle) {
                idle();
                flush();
            }
        }
    }
}

namespace {
struct Dlg {
    int x, y, w, h;
};

Dlg dialog_frame(const char* title, int lines, int min_w) {
    int w = min_w + 6;
    if (w > cols() - 4) w = cols() - 4;
    int h = lines + 4;
    Dlg d{(cols() - w) / 2, (rows() - h) / 2, w, h};
    box(d.x, d.y, d.w, d.h, White, BgDialog, title);
    return d;
}
}  // namespace

bool input(const char* title, const char* prompt, char* buf, size_t cap) {
    int pw = utf8_cols(prompt);
    int want = pw > 60 ? pw : 60;
    Dlg d = dialog_frame(title, 3, want);
    text(d.x + 3, d.y + 2, prompt, White, BgDialog, d.w - 6);
    text(d.x + 3, d.y + d.h - 2, "Enter: OK   Esc: отмена", Dim, BgDialog, d.w - 6);
    size_t len = strlen(buf);
    int fw = d.w - 6;
    for (;;) {
        fill(d.x + 3, d.y + 3, fw, 1, ' ', White, Black);
        // show the tail if too long
        const char* s = buf;
        int total = utf8_cols(buf);
        for (int skip = total - (fw - 1); skip > 0; skip--) utf8_next(s);
        int n = text(d.x + 3, d.y + 3, s, White, Black, fw - 1);
        cell(d.x + 3 + n, d.y + 3, ' ', Black, White);  // cursor
        flush();
        kbd::Event e = next_key();
        if (e.key == kbd::Esc) return false;
        if (e.key == kbd::Enter) return true;
        if (e.key == kbd::Backspace && len > 0) {
            do len--; while (len > 0 && (uint8_t(buf[len]) & 0xC0) == 0x80);
            buf[len] = 0;
        } else if (e.key == kbd::Char && len + 1 < cap) {
            buf[len++] = e.ch;
            buf[len] = 0;
        }
    }
}

bool confirm(const char* title, const char* question) {
    return choose(title, question, "y\n") != 0;
}

char choose(const char* title, const char* textline, const char* keys) {
    int tw = utf8_cols(textline);
    Dlg d = dialog_frame(title, 2, tw < 40 ? 40 : tw);
    text(d.x + 3, d.y + 2, textline, White, BgDialog, d.w - 6);
    if (keys[0] == 'y' && keys[1] == '\n')
        text(d.x + 3, d.y + 3, "Enter или Y: да    Esc: нет", Dim, BgDialog, d.w - 6);
    flush();
    for (;;) {
        kbd::Event e = next_key();
        if (e.key == kbd::Esc) return 0;
        char c = 0;
        if (e.key == kbd::Enter) c = '\n';
        else if (e.key == kbd::Char) c = (e.ch >= 'A' && e.ch <= 'Z') ? char(e.ch + 32) : e.ch;
        for (const char* k = keys; *k; k++)
            if (*k == c) return c == '\n' ? keys[0] : c;
    }
}

void message(const char* title, const char* textline) {
    int tw = utf8_cols(textline);
    Dlg d = dialog_frame(title, 2, tw < 40 ? 40 : tw);
    text(d.x + 3, d.y + 2, textline, White, BgDialog, d.w - 6);
    text(d.x + 3, d.y + 3, "Любая клавиша: закрыть", Dim, BgDialog, d.w - 6);
    flush();
    next_key();
}

}  // namespace ui
