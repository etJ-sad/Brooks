// SPDX-License-Identifier: GPL-2.0-only
// CMOS real-time clock (time as kept by the firmware, usually local time on
// machines that also run Windows).
#pragma once
#include <stdint.h>

namespace rtc {
struct Time {
    uint16_t year;
    uint8_t month, day, hour, min, sec;
};
bool read(Time& t);
}  // namespace rtc
