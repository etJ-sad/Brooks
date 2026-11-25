// SPDX-License-Identifier: GPL-2.0-only
// Read-only file viewer: UTF-8 text with horizontal scrolling, or hex dump.
#include "apps/apps.h"
#include "apps/ui.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"

namespace apps {

using namespace con;

namespace {

const char* const kKeys[10] = {"Помощь", nullptr, "Выход", "Hex", nullptr, nullptr, nullptr, nullptr, nullptr, "Выход"};

bool looks_binary(const uint8_t* d, size_t n) {
    size_t lim = n < 8192 ? n : 8192;
    for (size_t i = 0; i < lim; i++)
        if (d[i] == 0) return true;
    return false;
}

}  // namespace

void view(fs::Node* f) {
    if (!f || f->dir) return;
    const uint8_t* d = f->data;
    size_t n = f->size;
    bool hex = looks_binary(d, n);

    // Line start offsets for text mode.
    size_t nlines = 1;
    for (size_t i = 0; i < n; i++)
        if (d[i] == '\n') nlines++;
    auto* starts = static_cast<size_t*>(kmalloc(nlines * sizeof(size_t)));
    if (!starts) {
        ui::message("Просмотр", "Не хватает памяти");
        return;
    }
    size_t li = 0;
    starts[li++] = 0;
    for (size_t i = 0; i < n; i++)
        if (d[i] == '\n' && li < nlines) starts[li++] = i + 1;

    char path[fs::PATH_MAX];
    fs::path_of(f, path, sizeof(path));
    long top = 0;
    int left = 0;
    char* line = static_cast<char*>(kmalloc(4096));

    for (;;) {
        const int W = ui::cols(), H = ui::rows(), body = H - 2;
        size_t total = hex ? (n + 15) / 16 : nlines;
        if (top > long(total) - body) top = long(total) - body;
        if (top < 0) top = 0;

        char hdr[256], sz[32];
        ui::fmt_size(n, sz, sizeof(sz));
        ksnprintf(hdr, sizeof(hdr), "%s   %s   строки %ld-%ld из %lu   %s", path, sz, top + 1,
                  top + body < long(total) ? top + body : long(total), total, hex ? "HEX" : "текст");
        ui::topbar(hdr);
        for (int r = 0; r < body; r++) {
            long idx = top + r;
            fill(0, r + 1, W, 1, ' ', Default, Black);
            if (idx >= long(total)) continue;
            if (hex) {
                size_t off = size_t(idx) * 16;
                int p = ksnprintf(line, 4096, "%08lx  ", off);
                for (int k = 0; k < 16; k++) {
                    if (off + k < n) p += int(ksnprintf(line + p, 4096 - p, "%02x ", d[off + k]));
                    else p += int(ksnprintf(line + p, 4096 - p, "   "));
                    if (k == 7) line[p++] = ' ';
                }
                line[p++] = ' ';
                for (int k = 0; k < 16 && off + k < n; k++) {
                    uint8_t c = d[off + k];
                    line[p++] = (c >= 0x20 && c < 0x7F) ? char(c) : '.';
                }
                line[p] = 0;
                text(0, r + 1, line, Default, Black, W);
            } else {
                size_t s = starts[idx];
                size_t e = (size_t(idx) + 1 < nlines) ? starts[idx + 1] - 1 : n;
                if (e > s && d[e - 1] == '\r') e--;
                size_t l = e - s;
                if (l > 4095) l = 4095;
                memcpy(line, d + s, l);
                line[l] = 0;
                for (size_t k = 0; k < l; k++)
                    if (line[k] == '\t') line[k] = ' ';
                const char* p = line;
                for (int k = 0; k < left && utf8_next(p); k++) {}
                text(0, r + 1, p, Default, Black, W);
            }
        }
        ui::keybar(kKeys);
        flush();

        kbd::Event e = ui::next_key();
        switch (e.key) {
        case kbd::Esc: case kbd::F3: case kbd::F10: goto out;
        case kbd::Up: top--; break;
        case kbd::Down: top++; break;
        case kbd::PageUp: top -= body - 1; break;
        case kbd::PageDown: top += body - 1; break;
        case kbd::Home: top = 0; left = 0; break;
        case kbd::End: top = long(total); break;
        case kbd::Left: left = left >= 8 ? left - 8 : 0; break;
        case kbd::Right: left += 8; break;
        case kbd::F4: hex = !hex; top = 0; break;
        case kbd::Char:
            if (e.ch == 'q' || e.ch == '3' || e.ch == '0') goto out;
            if (e.ch == 'h' || e.ch == '4') { hex = !hex; top = 0; }
            if (e.ch == ' ') top += body - 1;
            break;
        default: break;
        }
    }
out:
    kfree(line);
    kfree(starts);
}

}  // namespace apps
