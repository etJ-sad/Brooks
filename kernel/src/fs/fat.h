// SPDX-License-Identifier: GPL-2.0-only
// FAT32 driver (long file names, read and write) as a VFS backend.
#pragma once
#include <stddef.h>
#include "block/block.h"

namespace fat {
// Mounts the volume as /<mname> if it is FAT32. The boot drive (contains
// /boot/limine/limine.conf) is read-write, others read-only unless all_rw.
bool probe_mount(const blk::Volume& v, const char* mname, bool all_rw, char* info, size_t cap);
}  // namespace fat
