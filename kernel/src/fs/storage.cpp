// SPDX-License-Identifier: GPL-2.0-only
#include "fs/storage.h"
#include "block/block.h"
#include "fs/exfat.h"
#include "fs/fat.h"
#include "fs/ntfs.h"
#include "lib/kprintf.h"
#include "usb/usb.h"

namespace storage {
namespace {
bool g_scanned[16];
int g_mounts = 0;
}  // namespace

int attach(void (*log)(const char*), bool all_rw) {
    usb::init(log);
    msc::probe_all(log);
    int mounted = 0;
    char line[240], info[160], mname[16];
    for (int d = 0; d < blk::device_count() && d < 16; d++) {
        if (g_scanned[d]) continue;
        g_scanned[d] = true;
        blk::Device* dev = blk::device(d);
        blk::Volume vols[16];
        int nv = blk::scan(dev, vols, 16);
        if (nv == 0) {
            vols[0] = {dev, 0, dev->sectors, 0, "whole disk"};
            nv = 1;
        }
        for (int i = 0; i < nv; i++) {
            ksnprintf(mname, sizeof(mname), "usb%d", g_mounts);
            bool ok = fat::probe_mount(vols[i], mname, all_rw, info, sizeof(info)) ||
                      exfat::probe_mount(vols[i], mname, all_rw, info, sizeof(info)) ||
                      ntfs::probe_mount(vols[i], mname, all_rw, info, sizeof(info));
            if (ok) {
                g_mounts++;
                mounted++;
                ksnprintf(line, sizeof(line), "  /%s: %s  [%s, part %d]", mname, info, dev->name, vols[i].index);
            } else {
                ksnprintf(line, sizeof(line), "  %s part %d (%s): unknown file system, skipped", dev->name,
                          vols[i].index, vols[i].type);
            }
            if (log) log(line);
        }
    }
    return mounted;
}

}  // namespace storage
