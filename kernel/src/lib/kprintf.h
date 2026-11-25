// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdarg.h>
#include "gfx/console.h"

// printf subset: %s %c %d %i %u %x %X %p %%, flags '-' '0', width, length l/ll/z.
void kvprintf(const char* fmt, va_list ap);
__attribute__((format(printf, 1, 2))) void kprintf(const char* fmt, ...);
// Same, but in the given colour; the previous colour is restored afterwards.
__attribute__((format(printf, 2, 3))) void kcprintf(con::Color c, const char* fmt, ...);
