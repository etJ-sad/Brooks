// SPDX-License-Identifier: GPL-2.0-only
// Brings up USB storage and mounts every recognised file system.
#pragma once

namespace storage {
// Safe to call more than once (new devices only). Returns mounted volumes.
int attach(void (*log)(const char* line), bool all_rw);
}
