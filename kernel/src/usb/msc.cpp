// SPDX-License-Identifier: GPL-2.0-only
// USB mass storage, Bulk-Only Transport + SCSI (READ/WRITE 10/16).
#include "block/block.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "usb/usb.h"

namespace msc {
namespace {

constexpr uint32_t XFER = 64 * 1024;  // bounce buffer, 64 KiB aligned

struct Msc {
    usb::Device* dev;
    uint8_t* buf;   // XFER bytes, does not cross a 64 KiB boundary
    uint8_t* cmd;   // CBW / CSW page
    uint32_t tag;
    uint8_t lun;
    bool use16;
    blk::Device blk;
};

void (*g_log)(const char*) = nullptr;
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* fmt, ...) {
    if (!g_log) return;
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    g_log(b);
}

void reset_recovery(Msc* m) {
    usb::SetupPacket rst{0x21, 0xFF, 0, m->dev->iface, 0};  // Bulk-Only Mass Storage Reset
    usb::control(m->dev, rst, nullptr);
    usb::clear_halt(m->dev, m->dev->bulk_in);
    usb::clear_halt(m->dev, m->dev->bulk_out);
}

// One SCSI command. dir_in: data phase direction. Returns true on good status.
bool scsi(Msc* m, const uint8_t* cdb, int cdb_len, bool dir_in, uint32_t len) {
    uint8_t* c = m->cmd;
    memset(c, 0, 31);
    uint32_t sig = 0x43425355, tag = ++m->tag;
    memcpy(c, &sig, 4);
    memcpy(c + 4, &tag, 4);
    memcpy(c + 8, &len, 4);
    c[12] = dir_in ? 0x80 : 0;
    c[13] = m->lun;
    c[14] = uint8_t(cdb_len);
    memcpy(c + 15, cdb, size_t(cdb_len));
    if (usb::bulk(m->dev, false, c, 31) != 31) {
        reset_recovery(m);
        return false;
    }
    if (len) {
        int r = usb::bulk(m->dev, dir_in, m->buf, len);
        if (r == -2) usb::clear_halt(m->dev, dir_in ? m->dev->bulk_in : m->dev->bulk_out);
        else if (r < 0) {
            reset_recovery(m);
            return false;
        }
    }
    uint8_t* s = m->cmd + 64;
    int r = usb::bulk(m->dev, true, s, 13);
    if (r == -2) {
        usb::clear_halt(m->dev, m->dev->bulk_in);
        r = usb::bulk(m->dev, true, s, 13);
    }
    uint32_t ssig;
    memcpy(&ssig, s, 4);
    if (r != 13 || ssig != 0x53425355) {
        reset_recovery(m);
        return false;
    }
    return s[12] == 0;
}

bool rw(Msc* m, bool write, uint64_t lba, uint32_t count) {
    uint8_t cdb[16] = {};
    uint32_t bytes = count * m->blk.sector_size;
    if (m->use16) {
        cdb[0] = write ? 0x8A : 0x88;
        for (int i = 0; i < 8; i++) cdb[2 + i] = uint8_t(lba >> (56 - 8 * i));
        cdb[10] = uint8_t(count >> 24); cdb[11] = uint8_t(count >> 16);
        cdb[12] = uint8_t(count >> 8); cdb[13] = uint8_t(count);
        return scsi(m, cdb, 16, !write, bytes);
    }
    cdb[0] = write ? 0x2A : 0x28;
    cdb[2] = uint8_t(lba >> 24); cdb[3] = uint8_t(lba >> 16); cdb[4] = uint8_t(lba >> 8); cdb[5] = uint8_t(lba);
    cdb[7] = uint8_t(count >> 8); cdb[8] = uint8_t(count);
    return scsi(m, cdb, 10, !write, bytes);
}

bool blk_read(blk::Device* b, uint64_t lba, uint32_t count, void* out) {
    auto* m = static_cast<Msc*>(b->priv);
    uint32_t per = XFER / b->sector_size;
    auto* o = static_cast<uint8_t*>(out);
    while (count) {
        uint32_t n = count < per ? count : per;
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) ok = rw(m, false, lba, n);
        if (!ok) return false;
        memcpy(o, m->buf, size_t(n) * b->sector_size);
        o += size_t(n) * b->sector_size;
        lba += n;
        count -= n;
    }
    return true;
}

bool blk_write(blk::Device* b, uint64_t lba, uint32_t count, const void* in) {
    auto* m = static_cast<Msc*>(b->priv);
    uint32_t per = XFER / b->sector_size;
    auto* p = static_cast<const uint8_t*>(in);
    while (count) {
        uint32_t n = count < per ? count : per;
        memcpy(m->buf, p, size_t(n) * b->sector_size);
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) ok = rw(m, true, lba, n);
        if (!ok) return false;
        p += size_t(n) * b->sector_size;
        lba += n;
        count -= n;
    }
    return true;
}

bool probe(usb::Device* d, int index) {
    if (!usb::configure_bulk(d)) return false;
    auto* m = static_cast<Msc*>(kzalloc(sizeof(Msc)));
    m->dev = d;
    // 128 KiB, use the 64 KiB-aligned half so no TRB crosses a 64 KiB boundary.
    uint64_t phys = mm::alloc_pages(32);
    uint64_t cphys = mm::alloc_pages(1);
    if (!phys || !cphys) return false;
    uint64_t aligned = (phys + XFER - 1) & ~uint64_t(XFER - 1);
    m->buf = mm::phys_to_virt<uint8_t>(aligned);
    m->cmd = mm::phys_to_virt<uint8_t>(cphys);

    usb::SetupPacket maxlun{0xA1, 0xFE, 0, d->iface, 1};
    uint8_t ml = 0;
    if (usb::control(d, maxlun, &ml) != 1) ml = 0;
    m->lun = 0;

    uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
    char vendor[9] = {}, product[17] = {};
    if (scsi(m, inq, 6, true, 36)) {
        memcpy(vendor, m->buf + 8, 8);
        memcpy(product, m->buf + 16, 16);
    }
    // Wait until the unit is ready (spin-up, SSD bridge init).
    bool ready = false;
    for (int i = 0; i < 20 && !ready; i++) {
        uint8_t tur[6] = {0, 0, 0, 0, 0, 0};
        ready = scsi(m, tur, 6, false, 0);
        if (!ready) {
            uint8_t rs[6] = {0x03, 0, 0, 0, 18, 0};
            scsi(m, rs, 6, true, 18);
            for (uint64_t k = 0; k < 5000000; k++) asm volatile("pause");
        }
    }
    uint8_t rc10[10] = {0x25};
    if (!scsi(m, rc10, 10, true, 8)) {
        logf("  USB %04x:%04x: READ CAPACITY failed", d->vid, d->pid);
        return false;
    }
    uint32_t last = uint32_t(m->buf[0] << 24 | m->buf[1] << 16 | m->buf[2] << 8 | m->buf[3]);
    uint32_t bs = uint32_t(m->buf[4] << 24 | m->buf[5] << 16 | m->buf[6] << 8 | m->buf[7]);
    uint64_t sectors = uint64_t(last) + 1;
    if (last == 0xFFFFFFFF) {
        uint8_t rc16[16] = {0x9E, 0x10};
        rc16[13] = 32;
        if (scsi(m, rc16, 16, true, 32)) {
            uint64_t l = 0;
            for (int i = 0; i < 8; i++) l = (l << 8) | m->buf[i];
            sectors = l + 1;
            bs = uint32_t(m->buf[8] << 24 | m->buf[9] << 16 | m->buf[10] << 8 | m->buf[11]);
        }
        m->use16 = true;
    }
    if (bs != 512 && bs != 4096) {
        logf("  USB %04x:%04x: unsupported sector size %u", d->vid, d->pid, bs);
        return false;
    }
    for (int i = 7; i >= 0 && vendor[i] == ' '; i--) vendor[i] = 0;
    for (int i = 15; i >= 0 && product[i] == ' '; i--) product[i] = 0;
    m->blk.sectors = sectors;
    m->blk.sector_size = bs;
    m->blk.read = blk_read;
    m->blk.write = blk_write;
    m->blk.priv = m;
    ksnprintf(m->blk.name, sizeof(m->blk.name), "usb%d %s %s", index, vendor, product);
    blk::register_device(&m->blk);
    logf("  usb%d: %s %s, %lu MiB, sector %u%s, max LUN %u", index, vendor, product,
         sectors * bs / (1024 * 1024), bs, m->use16 ? ", 16-byte CDB" : "", ml);
    return true;
}

}  // namespace

int probe_all(void (*log)(const char*)) {
    g_log = log;
    int n = 0;
    for (int i = 0; i < usb::device_count(); i++) {
        usb::Device* d = usb::device(i);
        if (d->bulk_in && d->bulk_out && !d->priv2_probed) {
            d->priv2_probed = true;
            if (probe(d, n)) n++;
        }
    }
    return n;
}

}  // namespace msc
