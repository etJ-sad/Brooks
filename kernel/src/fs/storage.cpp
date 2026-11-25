// SPDX-License-Identifier: GPL-2.0-only
#include "fs/storage.h"
#include "fs/fat.h"
#include "usb/usb.h"

namespace storage {

int attach(void (*log)(const char*), bool all_rw) {
    usb::init(log);
    msc::probe_all(log);
    return fat::mount_all(log, all_rw);
}

}  // namespace storage
