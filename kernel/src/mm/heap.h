// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stddef.h>

void* kmalloc(size_t n);
void* kzalloc(size_t n);
void* krealloc(void* p, size_t n);
void kfree(void* p);
size_t heap_used();
