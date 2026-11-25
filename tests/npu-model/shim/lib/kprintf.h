#pragma once
#include <stdio.h>
#include <stdarg.h>
inline size_t kvsnprintf(char* b, size_t c, const char* f, va_list ap) { return vsnprintf(b, c, f, ap); }
