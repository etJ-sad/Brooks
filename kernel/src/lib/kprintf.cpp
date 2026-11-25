// SPDX-License-Identifier: GPL-2.0-only
#include "lib/kprintf.h"
#include "lib/string.h"
#include <stdint.h>

namespace {

struct Sink {
    char* buf;  // nullptr -> console
    size_t cap, len;
    void put(char c) {
        if (!buf) {
            con::putc(c);
            return;
        }
        if (len + 1 < cap) buf[len] = c;
        len++;
    }
    void write(const char* s, size_t n) {
        for (size_t i = 0; i < n; i++) put(s[i]);
    }
};

void emit_padded(Sink& out, const char* s, size_t n, int width, bool left, char pad) {
    // Width counts code points, so UTF-8 text pads correctly.
    int cols = 0;
    for (size_t i = 0; i < n; i++)
        if ((uint8_t(s[i]) & 0xC0) != 0x80) cols++;
    int fill = width > cols ? width - cols : 0;
    if (!left && pad == '0' && n > 0 && s[0] == '-') {  // keep sign before zeros
        out.put('-');
        s++;
        n--;
    }
    if (!left)
        for (int i = 0; i < fill; i++) out.put(pad);
    out.write(s, n);
    if (left)
        for (int i = 0; i < fill; i++) out.put(' ');
}

size_t utoa(uint64_t v, unsigned base, bool upper, char* buf) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[32];
    size_t n = 0;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    for (size_t i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

}  // namespace

static void format(Sink& out, const char* fmt, va_list ap) {
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            out.put(*p);
            continue;
        }
        p++;
        bool left = false;
        char pad = ' ';
        for (;; p++) {
            if (*p == '-') left = true;
            else if (*p == '0') pad = '0';
            else break;
        }
        int width = 0;
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int lng = 0;
        while (*p == 'l' || *p == 'z') { lng++; p++; }

        char buf[40];
        size_t n = 0;
        switch (*p) {
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            emit_padded(out, s, strlen(s), width, left, ' ');
            break;
        }
        case 'c':
            buf[0] = char(va_arg(ap, int));
            emit_padded(out, buf, 1, width, left, ' ');
            break;
        case 'd':
        case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            uint64_t u = v < 0 ? uint64_t(-(v + 1)) + 1 : uint64_t(v);
            if (v < 0) buf[n++] = '-';
            n += utoa(u, 10, false, buf + n);
            emit_padded(out, buf, n, width, left, pad);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            n = utoa(v, *p == 'u' ? 10 : 16, *p == 'X', buf);
            emit_padded(out, buf, n, width, left, pad);
            break;
        }
        case 'p': {
            uintptr_t v = uintptr_t(va_arg(ap, void*));
            buf[0] = '0';
            buf[1] = 'x';
            n = 2 + utoa(v, 16, false, buf + 2);
            emit_padded(out, buf, n, width, left, ' ');
            break;
        }
        case '%':
            out.put('%');
            break;
        case 0:
            return;
        default:
            out.put('%');
            out.put(*p);
        }
    }
}

void kvprintf(const char* fmt, va_list ap) {
    Sink s{nullptr, 0, 0};
    format(s, fmt, ap);
}

size_t kvsnprintf(char* buf, size_t cap, const char* fmt, va_list ap) {
    Sink s{buf, cap, 0};
    format(s, fmt, ap);
    if (cap) buf[s.len < cap ? s.len : cap - 1] = 0;
    return s.len;
}

size_t ksnprintf(char* buf, size_t cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t n = kvsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

void kprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

void kcprintf(con::Color c, const char* fmt, ...) {
    con::Color old = con::color();
    con::set_color(c);
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    con::set_color(old);
}
