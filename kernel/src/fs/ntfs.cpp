// SPDX-License-Identifier: GPL-2.0-only
#include "fs/ntfs.h"
namespace ntfs {
bool probe_mount(const blk::Volume&, const char*, bool, char*, size_t) { return false; }
}
