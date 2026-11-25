// SPDX-License-Identifier: GPL-2.0-only
// Intel NPU (Meteor Lake / Arrow Lake, NPU 37xx IP with the MTL buttress).
// Step A of the bring-up: power the NPU up and down the way Linux ivpu does
// (drivers/accel/ivpu: ivpu_hw.c, ivpu_hw_btrs.c, ivpu_hw_ip.c), logging every
// register on the way. No firmware is loaded yet.
#pragma once
#include <stddef.h>

namespace npu {

// log() gets one line per step. Returns true if power-up and power-down
// both completed. Also writes /sys/npu/bringup.txt.
bool bringup(void (*log)(const char*));

}  // namespace npu
