#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
namespace mm {
constexpr uint64_t PAGE = 4096;
inline uint64_t hhdm() { return 0; }
template <typename T = void> inline T* phys_to_virt(uint64_t p) { return reinterpret_cast<T*>(p); }
extern size_t g_live;
inline uint64_t alloc_pages(size_t n) { void* p = aligned_alloc(4096, n * 4096); memset(p, 0, n * 4096); g_live += n; return uint64_t(p); }
inline void free_pages(uint64_t p, size_t n) { free((void*)p); g_live -= n; }
}
