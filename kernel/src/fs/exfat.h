// SPDX-License-Identifier: GPL-2.0-only
// exFAT driver (read and write) as a VFS backend.
#pragma once
#include <stddef.h>
#include "block/block.h"

namespace exfat {
bool probe_mount(const blk::Volume& v, const char* mname, bool all_rw, char* info, size_t cap);
}
