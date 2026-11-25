// SPDX-License-Identifier: GPL-2.0-only
// Built-in apps. They run in the kernel until Phase 1 brings processes and
// user space; then they move to userspace/ (MIT).
#pragma once
#include "fs/vfs.h"

namespace apps {
void view(fs::Node* f);
void edit(fs::Node* f);
[[noreturn]] void file_manager();
}  // namespace apps
