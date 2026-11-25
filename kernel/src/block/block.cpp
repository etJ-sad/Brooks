// SPDX-License-Identifier: GPL-2.0-only
#include "block/block.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"

namespace blk {
namespace {
Device* g_devs[16];
int g_ndevs = 0;
}  // namespace

void register_device(Device* d) {
    if (g_ndevs < 16) g_devs[g_ndevs++] = d;
}
int device_count() { return g_ndevs; }
Device* device(int i) { return (i >= 0 && i < g_ndevs) ? g_devs[i] : nullptr; }

bool read(const Volume& v, uint64_t lba, uint32_t count, void* buf) {
    if (lba + count > v.sectors) return false;
    return v.dev->read(v.dev, v.start + lba, count, buf);
}

bool write(const Volume& v, uint64_t lba, uint32_t count, const void* buf) {
    if (lba + count > v.sectors) return false;
    return v.dev->write(v.dev, v.start + lba, count, buf);
}

int scan(Device* d, Volume* out, int max) {
    const uint32_t ss = d->sector_size;
    auto* buf = static_cast<uint8_t*>(kmalloc(ss * 2 > 16384 ? ss * 2 : 16384));
    if (!buf) return 0;
    int n = 0;
    // GPT header at LBA 1.
    if (d->read(d, 1, 1, buf) && memcmp(buf, "EFI PART", 8) == 0) {
        uint64_t ent_lba;
        uint32_t nents, esz;
        memcpy(&ent_lba, buf + 72, 8);
        memcpy(&nents, buf + 80, 4);
        memcpy(&esz, buf + 84, 4);
        if (esz >= 128 && esz <= 1024 && nents <= 1024) {
            uint32_t per = ss / esz;
            for (uint32_t i = 0; i < nents && n < max; i++) {
                if (i % per == 0 && !d->read(d, ent_lba + i / per, 1, buf)) break;
                const uint8_t* e = buf + (i % per) * esz;
                bool empty = true;
                for (int k = 0; k < 16; k++)
                    if (e[k]) empty = false;
                if (empty) continue;
                uint64_t first, last;
                memcpy(&first, e + 32, 8);
                memcpy(&last, e + 40, 8);
                if (last < first || last >= d->sectors) continue;
                Volume& v = out[n++];
                v = {d, first, last - first + 1, int(i + 1), {}};
                static const uint8_t esp[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                                                0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};
                static const uint8_t basic[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                                                  0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
                const char* t = memcmp(e, esp, 16) == 0 ? "EFI System" : memcmp(e, basic, 16) == 0 ? "Basic data" : "GPT other";
                memcpy(v.type, t, strlen(t) + 1);
            }
            kfree(buf);
            return n;
        }
    }
    // MBR.
    if (d->read(d, 0, 1, buf) && buf[510] == 0x55 && buf[511] == 0xAA) {
        bool any = false;
        for (int i = 0; i < 4 && n < max; i++) {
            const uint8_t* e = buf + 446 + i * 16;
            uint8_t type = e[4];
            uint32_t start, count;
            memcpy(&start, e + 8, 4);
            memcpy(&count, e + 12, 4);
            if (!type || !count || type == 0xEE) continue;
            any = true;
            Volume& v = out[n++];
            v = {d, start, count, i + 1, {}};
            ksnprintf(v.type, sizeof(v.type), "MBR 0x%02x", type);
        }
        // A bare FAT volume (no partition table) also has 55AA: treat as whole disk.
        if (!any && n < max && (memcmp(buf + 82, "FAT32", 5) == 0 || memcmp(buf + 54, "FAT", 3) == 0)) {
            out[n++] = {d, 0, d->sectors, 0, "whole disk"};
        }
    }
    kfree(buf);
    return n;
}

}  // namespace blk
