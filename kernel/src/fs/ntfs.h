// SPDX-License-Identifier: GPL-2.0-only
// NTFS driver (read-only) as a VFS backend.
#pragma once
#include <stddef.h>
#include "block/block.h"

namespace ntfs {
bool probe_mount(const blk::Volume& v, const char* mname, bool all_rw, char* info, size_t cap);
}
