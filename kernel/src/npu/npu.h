// SPDX-License-Identifier: GPL-2.0-only
// Intel NPU (Meteor Lake / Arrow Lake, NPU 37xx IP with the MTL buttress),
// brought up the way Linux ivpu does (drivers/accel/ivpu), logging every step:
//   step A: power up (workpoint, power island, NOC) and down;
//   step B: NPU MMU, firmware vpu_37xx_v1.bin, boot parameters, start, "ready" message.
#pragma once
#include <stddef.h>

namespace npu {

struct Result {
    bool power_up = false, power_down = false;
    bool fw_tried = false, fw_ready = false;
};

// log() gets one line per step. With start_fw the firmware is started between
// power up and power down. Also writes /sys/npu/bringup.txt (and the firmware
// logs /sys/npu/fw-*.txt).
Result bringup(void (*log)(const char*), bool start_fw);

}  // namespace npu
