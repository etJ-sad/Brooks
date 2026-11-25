// SPDX-License-Identifier: GPL-2.0-only
// Minimal text editor (UTF-8, LF line endings).
#include "apps/apps.h"
#include "apps/ui.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"

namespace apps {

using namespace con;

namespace {

struct Line {
    char* s;
    int len, cap;
};

struct Doc {
    Line* lines = nullptr;
    int n = 0, cap = 0;
    bool dirty = false;
};

bool reserve(Line& l, int need) {
    if (need + 1 <= l.cap) return true;
    int cap = l.cap ? l.cap : 32;
    while (cap < need + 1) cap *= 2;
    char* p = static_cast<char*>(krealloc(l.s, size_t(cap)));
    if (!p) return false;
    l.s = p;
    l.cap = cap;
    return true;
}

bool insert_line(Doc& d, int at, const char* s, int len) {
    if (d.n + 1 > d.cap) {
        int cap = d.cap ? d.cap * 2 : 64;
        auto* p = static_cast<Line*>(krealloc(d.lines, sizeof(Line) * size_t(cap)));
        if (!p) return false;
        d.lines = p;
        d.cap = cap;
    }
    for (int i = d.n; i > at; i--) d.lines[i] = d.lines[i - 1];
    d.lines[at] = {nullptr, 0, 0};
    d.n++;
    if (!reserve(d.lines[at], len)) return false;
    memcpy(d.lines[at].s, s, size_t(len));
    d.lines[at].len = len;
    d.lines[at].s[len] = 0;
    return true;
}

void erase_line(Doc& d, int at) {
    kfree(d.lines[at].s);
    for (int i = at; i < d.n - 1; i++) d.lines[i] = d.lines[i + 1];
    d.n--;
}

void free_doc(Doc& d) {
    for (int i = 0; i < d.n; i++) kfree(d.lines[i].s);
    kfree(d.lines);
}

bool save(Doc& d, fs::Node* f) {
    size_t total = 0;
    for (int i = 0; i < d.n; i++) total += size_t(d.lines[i].len) + 1;
    char* buf = static_cast<char*>(kmalloc(total + 1));
    if (!buf) return false;
    size_t p = 0;
    for (int i = 0; i < d.n; i++) {
        memcpy(buf + p, d.lines[i].s, size_t(d.lines[i].len));
        p += size_t(d.lines[i].len);
        if (i + 1 < d.n) buf[p++] = '\n';
    }
    bool ok = fs::write(f, buf, p) == fs::Ok;
    kfree(buf);
    if (ok) d.dirty = false;
    return ok;
}

int cps_before(const Line& l, int byte) {
    int c = 0;
    for (int i = 0; i < byte && i < l.len; i++)
        if ((uint8_t(l.s[i]) & 0xC0) != 0x80) c++;
    return c;
}

int byte_at_cp(const Line& l, int cp) {
    int c = 0, i = 0;
    while (i < l.len && c < cp) {
        i++;
        while (i < l.len && (uint8_t(l.s[i]) & 0xC0) == 0x80) i++;
        c++;
    }
    return i;
}

const char* const kKeys[10] = {"Помощь", "Сохран", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "Выход"};

}  // namespace

void edit(fs::Node* f) {
    if (!f || f->dir) return;
    if (!fs::ensure_data(f)) {
        ui::message("Редактор", "Не удалось прочитать файл с диска");
        return;
    }
    Doc d;
    {
        size_t s = 0;
        for (size_t i = 0; i <= f->size; i++) {
            if (i == f->size || f->data[i] == '\n') {
                size_t e = i;
                if (e > s && f->data[e - 1] == '\r') e--;
                if (!insert_line(d, d.n, reinterpret_cast<const char*>(f->data) + s, int(e - s))) {
                    free_doc(d);
                    ui::message("Редактор", "Не хватает памяти");
                    return;
                }
                s = i + 1;
            }
        }
    }
    char path[fs::PATH_MAX];
    fs::path_of(f, path, sizeof(path));
    int cy = 0, cx = 0, top = 0, left = 0, want_col = 0;
    char status[160] = "";

    for (;;) {
        const int W = ui::cols(), H = ui::rows(), body = H - 2;
        int ccol = cps_before(d.lines[cy], cx);
        if (cy < top) top = cy;
        if (cy >= top + body) top = cy - body + 1;
        if (ccol < left) left = ccol;
        if (ccol >= left + W - 1) left = ccol - W + 2;

        char hdr[300];
        ksnprintf(hdr, sizeof(hdr), "%s%s   стр %d/%d  кол %d   %s", path, d.dirty ? " *" : "", cy + 1, d.n,
                  ccol + 1, status);
        ui::topbar(hdr);
        for (int r = 0; r < body; r++) {
            int idx = top + r;
            fill(0, r + 1, W, 1, ' ', Default, BgPanel);
            if (idx >= d.n) {
                cell(0, r + 1, '~', Dim, BgPanel);
                continue;
            }
            const Line& l = d.lines[idx];
            const char* p = l.s;
            for (int k = 0; k < left && utf8_next(p); k++) {}
            text(0, r + 1, p, White, BgPanel, W);
        }
        // cursor
        {
            const Line& l = d.lines[cy];
            const char* p = l.s + cx;
            uint32_t cp = cx < l.len ? utf8_next(p) : ' ';
            cell(ccol - left, cy - top + 1, cp, Black, Yellow);
        }
        ui::keybar(kKeys);
        flush();
        status[0] = 0;

        kbd::Event e = ui::next_key();
        Line& L = d.lines[cy];
        bool vertical = false;
        switch (e.key) {
        case kbd::Esc:
        case kbd::F10: {
            if (!d.dirty) goto out;
            char c = ui::choose("Редактор", "Сохранить изменения? Y: да, N: нет, Esc: вернуться", "yn");
            if (c == 'y' && save(d, f)) goto out;
            if (c == 'n') goto out;
            break;
        }
        case kbd::F2:
            ksnprintf(status, sizeof(status), save(d, f) ? "сохранено" : "ошибка сохранения");
            break;
        case kbd::Up: if (cy > 0) cy--; vertical = true; break;
        case kbd::Down: if (cy + 1 < d.n) cy++; vertical = true; break;
        case kbd::PageUp: cy = cy > body ? cy - body : 0; vertical = true; break;
        case kbd::PageDown: cy = cy + body < d.n ? cy + body : d.n - 1; vertical = true; break;
        case kbd::Left:
            if (cx > 0) {
                do cx--; while (cx > 0 && (uint8_t(L.s[cx]) & 0xC0) == 0x80);
            } else if (cy > 0) {
                cy--;
                cx = d.lines[cy].len;
            }
            break;
        case kbd::Right:
            if (cx < L.len) {
                do cx++; while (cx < L.len && (uint8_t(L.s[cx]) & 0xC0) == 0x80);
            } else if (cy + 1 < d.n) {
                cy++;
                cx = 0;
            }
            break;
        case kbd::Home: cx = 0; break;
        case kbd::End: cx = L.len; break;
        case kbd::Enter: {
            if (insert_line(d, cy + 1, d.lines[cy].s + cx, d.lines[cy].len - cx)) {
                d.lines[cy].len = cx;
                d.lines[cy].s[cx] = 0;
                cy++;
                cx = 0;
                d.dirty = true;
            }
            break;
        }
        case kbd::Backspace:
            if (cx > 0) {
                int s = cx;
                do s--; while (s > 0 && (uint8_t(L.s[s]) & 0xC0) == 0x80);
                memmove(L.s + s, L.s + cx, size_t(L.len - cx + 1));
                L.len -= cx - s;
                cx = s;
                d.dirty = true;
            } else if (cy > 0) {
                Line& P = d.lines[cy - 1];
                int plen = P.len;
                if (reserve(P, P.len + L.len)) {
                    memcpy(P.s + P.len, L.s, size_t(L.len) + 1);
                    P.len += L.len;
                    erase_line(d, cy);
                    cy--;
                    cx = plen;
                    d.dirty = true;
                }
            }
            break;
        case kbd::Delete:
            if (cx < L.len) {
                int s = cx + 1;
                while (s < L.len && (uint8_t(L.s[s]) & 0xC0) == 0x80) s++;
                memmove(L.s + cx, L.s + s, size_t(L.len - s + 1));
                L.len -= s - cx;
                d.dirty = true;
            } else if (cy + 1 < d.n) {
                Line& N = d.lines[cy + 1];
                if (reserve(L, L.len + N.len)) {
                    memcpy(L.s + L.len, N.s, size_t(N.len) + 1);
                    L.len += N.len;
                    erase_line(d, cy + 1);
                    d.dirty = true;
                }
            }
            break;
        case kbd::Tab:
        case kbd::Char: {
            if (e.key == kbd::Char && (e.mods & kbd::Ctrl)) {
                if (e.ch == 's' || e.ch == 'S')
                    ksnprintf(status, sizeof(status), save(d, f) ? "сохранено" : "ошибка сохранения");
                if (e.ch == 'q' || e.ch == 'Q') {
                    if (!d.dirty) goto out;
                }
                break;
            }
            const char* ins = e.key == kbd::Tab ? "    " : &e.ch;
            int n = e.key == kbd::Tab ? 4 : 1;
            if (reserve(L, L.len + n)) {
                memmove(L.s + cx + n, L.s + cx, size_t(L.len - cx + 1));
                memcpy(L.s + cx, ins, size_t(n));
                L.len += n;
                cx += n;
                d.dirty = true;
            }
            break;
        }
        default: break;
        }
        if (vertical) cx = byte_at_cp(d.lines[cy], want_col);
        else want_col = cps_before(d.lines[cy], cx);
    }
out:
    free_doc(d);
}

}  // namespace apps
