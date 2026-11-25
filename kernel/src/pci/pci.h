// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>

namespace pci {

struct Device {
    uint8_t bus, dev, fn;
    uint16_t vendor, device;
    uint8_t cls, subcls, progif, rev;
    uint8_t header_type;
    uint16_t subsys_vendor, subsys_id;
};

uint32_t read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);

// Brute-force scan of every bus/device/function via legacy port I/O
// (0xCF8/0xCFC). Fills the internal table and returns the number of devices.
int scan();
int count();
const Device& at(int i);

const char* vendor_name(uint16_t vendor);
const char* class_name(uint8_t cls, uint8_t subcls, uint8_t progif);
// Returns the generation name ("Arrow Lake NPU" ...) if this is an Intel NPU, else nullptr.
const char* intel_npu_name(const Device& d);

}  // namespace pci
