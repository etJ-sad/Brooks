#pragma once
#include <stdint.h>
namespace pci { struct Device { uint8_t bus, dev, fn; uint16_t vendor, device; uint8_t rev; };
inline Device g_d{0, 0x0b, 0, 0x8086, 0xad1d, 1};
inline int count() { return 1; } inline const Device& at(int) { return g_d; }
inline const char* intel_npu_name(const Device&) { return "Arrow Lake NPU"; }
inline uint32_t read32(uint8_t, uint8_t, uint8_t, uint8_t) { return 0; }
inline void write32(uint8_t, uint8_t, uint8_t, uint8_t, uint32_t) {}
inline void enable_mmio_dma(const Device&) {}
inline uint64_t bar_address(const Device&, int b) { return b == 0 ? 0x10000000 : 0x2000; } }
