// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stddef.h>

extern "C" {
void* memcpy(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
void* memmove(void* dst, const void* src, size_t n);
int memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
}

namespace kstr {
// True if `hay` contains `key` followed by '=' and writes the value (up to space) into out.
bool cmdline_get(const char* hay, const char* key, char* out, size_t out_len);
long to_long(const char* s, long fallback);
}  // namespace kstr
