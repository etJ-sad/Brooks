// SPDX-License-Identifier: GPL-2.0-only
// Freestanding memory primitives. The compiler may emit calls to these.
#include <stddef.h>
#include <stdint.h>

extern "C" {

void* memcpy(void* dst, const void* src, size_t n) {
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    asm volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n)::"memory");
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    auto* d = static_cast<uint8_t*>(dst);
    asm volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) return memcpy(dst, src, n);
    for (size_t i = n; i-- > 0;) d[i] = s[i];
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    auto* x = static_cast<const uint8_t*>(a);
    auto* y = static_cast<const uint8_t*>(b);
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

}  // extern "C"
