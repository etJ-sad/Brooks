// SPDX-License-Identifier: GPL-2.0-only
// FAT32 driver (long file names, read and write) as a VFS backend.
#pragma once

namespace fat {
// Mount every FAT32 volume on registered block devices as /usb0, /usb1, ...
// Volumes that contain /boot/limine/limine.conf (our boot drive) are mounted
// read-write, others read-only unless all_rw is set.
int mount_all(void (*log)(const char* line), bool all_rw);
}  // namespace fat
