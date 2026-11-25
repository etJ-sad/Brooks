#pragma once
#include <x86intrin.h>
namespace cpu { inline uint64_t rdtsc() { return __rdtsc(); } inline void pause() { _mm_pause(); } }
