// SPDX-License-Identifier: GPL-2.0-only
#include "lib/string.h"

namespace kstr {

bool cmdline_get(const char* hay, const char* key, char* out, size_t out_len) {
    if (!hay || !key || out_len == 0) return false;
    size_t kl = strlen(key);
    for (const char* p = hay; *p; p++) {
        bool at_word = (p == hay) || p[-1] == ' ';
        if (!at_word || memcmp(p, key, kl) != 0 || p[kl] != '=') continue;
        const char* v = p + kl + 1;
        size_t i = 0;
        while (v[i] && v[i] != ' ' && i + 1 < out_len) { out[i] = v[i]; i++; }
        out[i] = 0;
        return true;
    }
    return false;
}

long to_long(const char* s, long fallback) {
    if (!s || !*s) return fallback;
    long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return fallback;
        v = v * 10 + (*s - '0');
    }
    return v;
}

}  // namespace kstr
