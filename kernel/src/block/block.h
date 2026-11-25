// SPDX-License-Identifier: GPL-2.0-only
// Block devices and partitions.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace blk {

struct Device {
    char name[48];
    uint64_t sectors;
    uint32_t sector_size;
    bool (*read)(Device*, uint64_t lba, uint32_t count, void* buf);
    bool (*write)(Device*, uint64_t lba, uint32_t count, const void* buf);
    void* priv;
};

// A slice of a device (partition or whole disk) with sector-granular I/O.
struct Volume {
    Device* dev;
    uint64_t start, sectors;
    int index;          // partition number, 0 = whole disk
    char type[24];      // "EFI System", "Basic data", "MBR 0x0c" ...
};

void register_device(Device* d);
int device_count();
Device* device(int i);

// Partition scan (GPT, then MBR). Returns number of volumes written.
int scan(Device* d, Volume* out, int max);

bool read(const Volume& v, uint64_t lba, uint32_t count, void* buf);
bool write(const Volume& v, uint64_t lba, uint32_t count, const void* buf);

}  // namespace blk
