// SPDX-License-Identifier: GPL-2.0-only
// Firmware load and start for NPU 37xx (Meteor Lake / Arrow Lake), after
// Linux ivpu_fw.c, ivpu_hw_ip.c (soc_cpu_boot_37xx) and ivpu_drv.c (wait_for_ready).
#pragma once
#include <stdint.h>

namespace npu::boot {

struct Device {
    uint8_t bus;
    uint16_t device_id;
    uint8_t revision;
    uint8_t min_ratio, pn_ratio, max_ratio;
};

// Called with the NPU powered up. Sets up the NPU MMU, copies the firmware,
// fills the boot parameters, starts the NPU's CPU and waits for its "ready"
// message. Returns true once the firmware answered.
bool run(const Device& dev);

// Where the firmware is looked for, first hit wins.
extern const char* const FW_PATHS[];

}  // namespace npu::boot
