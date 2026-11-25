// SPDX-License-Identifier: GPL-2.0-only
#include "lib/kprintf.h"
#include "lib/string.h"
#include <stdint.h>

namespace {

void emit_padded(const char* s, size_t n, int width, bool left, char pad) {
    int fill = width > int(n) ? width - int(n) : 0;
    if (!left && pad == '0' && n > 0 && s[0] == '-') {  // keep sign before zeros
        con::putc('-');
        s++;
        n--;
    }
    if (!left)
        for (int i = 0; i < fill; i++) con::putc(pad);
    con::write(s, n);
    if (left)
        for (int i = 0; i < fill; i++) con::putc(' ');
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

void kvprintf(const char* fmt, va_list ap) {
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            con::putc(*p);
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
            emit_padded(s, strlen(s), width, left, ' ');
            break;
        }
        case 'c':
            buf[0] = char(va_arg(ap, int));
            emit_padded(buf, 1, width, left, ' ');
            break;
        case 'd':
        case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            uint64_t u = v < 0 ? uint64_t(-(v + 1)) + 1 : uint64_t(v);
            if (v < 0) buf[n++] = '-';
            n += utoa(u, 10, false, buf + n);
            emit_padded(buf, n, width, left, pad);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            n = utoa(v, *p == 'u' ? 10 : 16, *p == 'X', buf);
            emit_padded(buf, n, width, left, pad);
            break;
        }
        case 'p': {
            uintptr_t v = uintptr_t(va_arg(ap, void*));
            buf[0] = '0';
            buf[1] = 'x';
            n = 2 + utoa(v, 16, false, buf + 2);
            emit_padded(buf, n, width, left, ' ');
            break;
        }
        case '%':
            con::putc('%');
            break;
        case 0:
            return;
        default:
            con::putc('%');
            con::putc(*p);
        }
    }
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
