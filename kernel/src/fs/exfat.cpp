// SPDX-License-Identifier: GPL-2.0-only
// exFAT: allocation bitmap, FAT chains and contiguous (NoFatChain) files,
// file/stream/name entry sets with checksums and name hashes. New data is
// always written as FAT-chained clusters.
#include "fs/exfat.h"
#include "drivers/rtc.h"
#include "fs/vfs.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"

namespace exfat {
namespace {


struct X {
    fs::MountBase base;
    blk::Volume vol;
    uint32_t bps, spc, cbytes;
    uint32_t fat_off, heap_off, nclusters, root;
    uint32_t* bm_clusters;  // clusters of the allocation bitmap
    uint32_t bm_nclusters;
    uint8_t* bitmap;        // whole bitmap in RAM
    uint32_t hint;
    uint8_t* fatbuf;
    uint32_t fatbuf_sec;
    bool fat_dirty;
    uint8_t* secbuf;
    uint64_t secbuf_lba;
    uint8_t* cbuf;
    char label[48];
};

X* M(fs::Node* n) { return static_cast<X*>(n->mnt); }

struct Chain {
    uint32_t first;
    bool contig;
    uint64_t len;  // bytes; 0 = follow FAT until end
};

// ---------------------------------------------------------------- low level

uint64_t clus_lba(X* x, uint32_t c) { return x->heap_off + uint64_t(c - 2) * x->spc; }
bool read_clus(X* x, uint32_t c, void* b) { return blk::read(x->vol, clus_lba(x, c), x->spc, b); }
bool write_clus(X* x, uint32_t c, const void* b) {
    x->secbuf_lba = ~0ull;
    return blk::write(x->vol, clus_lba(x, c), x->spc, b);
}

bool fat_flush(X* x) {
    if (!x->fat_dirty) return true;
    if (!blk::write(x->vol, x->fat_off + x->fatbuf_sec, 1, x->fatbuf)) return false;
    x->fat_dirty = false;
    return true;
}

bool fat_load(X* x, uint32_t sec) {
    if (x->fatbuf_sec == sec) return true;
    if (!fat_flush(x)) return false;
    if (!blk::read(x->vol, x->fat_off + sec, 1, x->fatbuf)) {
        x->fatbuf_sec = 0xFFFFFFFF;
        return false;
    }
    x->fatbuf_sec = sec;
    return true;
}

uint32_t fat_get(X* x, uint32_t c) {
    if (!fat_load(x, (c * 4) / x->bps)) return 0xFFFFFFFF;
    uint32_t v;
    memcpy(&v, x->fatbuf + (c * 4) % x->bps, 4);
    return v;
}

bool fat_set(X* x, uint32_t c, uint32_t v) {
    if (!fat_load(x, (c * 4) / x->bps)) return false;
    memcpy(x->fatbuf + (c * 4) % x->bps, &v, 4);
    x->fat_dirty = true;
    return true;
}

uint32_t chain_next(X* x, const Chain& ch, uint32_t c, uint32_t i) {
    if (ch.contig) {
        uint64_t n = (ch.len + x->cbytes - 1) / x->cbytes;
        return (i + 1 < n) ? c + 1 : 0xFFFFFFFF;
    }
    return fat_get(x, c);
}

bool valid_clus(X* x, uint32_t c) { return c >= 2 && c < x->nclusters + 2; }

// Allocation bitmap ------------------------------------------------------------

bool bm_write_byte(X* x, uint32_t byte) {
    uint32_t per = x->cbytes;
    uint32_t ci = byte / per;
    if (ci >= x->bm_nclusters) return false;
    uint32_t in = byte % per;
    uint32_t sec_in = in / x->bps;
    uint64_t lba = clus_lba(x, x->bm_clusters[ci]) + sec_in;
    x->secbuf_lba = ~0ull;
    return blk::write(x->vol, lba, 1, x->bitmap + (uint64_t(ci) * per + uint64_t(sec_in) * x->bps));
}

bool bm_set(X* x, uint32_t c, bool used) {
    uint32_t bit = c - 2;
    uint8_t mask = uint8_t(1u << (bit & 7));
    if (used) x->bitmap[bit / 8] |= mask;
    else x->bitmap[bit / 8] &= uint8_t(~mask);
    return bm_write_byte(x, bit / 8);
}

bool bm_used(X* x, uint32_t c) { return x->bitmap[(c - 2) / 8] & (1u << ((c - 2) & 7)); }

uint32_t alloc_one(X* x) {
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t start = pass ? 2 : x->hint, end = pass ? x->hint : x->nclusters + 2;
        for (uint32_t c = start; c < end; c++) {
            if (!bm_used(x, c)) {
                if (!bm_set(x, c, true) || !fat_set(x, c, 0xFFFFFFFF)) return 0;
                x->hint = c + 1 < x->nclusters + 2 ? c + 1 : 2;
                return c;
            }
        }
    }
    return 0;
}

void free_chain(X* x, const Chain& ch) {
    uint32_t c = ch.first;
    for (uint32_t i = 0; valid_clus(x, c) && i < x->nclusters; i++) {
        uint32_t next = chain_next(x, ch, c, i);
        bm_set(x, c, false);
        if (!ch.contig) fat_set(x, c, 0);
        c = next;
    }
}

uint32_t alloc_chain(X* x, uint32_t n) {
    uint32_t first = 0, prev = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = alloc_one(x);
        if (!c) {
            free_chain(x, {first, false, 0});
            return 0;
        }
        if (prev) fat_set(x, prev, c);
        else first = c;
        prev = c;
    }
    return first;
}

// Directory entries --------------------------------------------------------------

Chain dir_chain(fs::Node* d) {
    return {uint32_t(d->fs_a), bool((d->fs_c >> 8) & 1), d->fs_c >> 16};
}

bool entry_pos(X* x, const Chain& ch, uint32_t idx, uint64_t& lba, uint32_t& off) {
    uint32_t per = x->cbytes / 32;
    uint32_t k = idx / per;
    uint32_t c = ch.first;
    if (ch.contig) {
        if (ch.len && uint64_t(k) * x->cbytes >= ch.len) return false;
        c += k;
    } else {
        for (uint32_t i = 0; i < k; i++) {
            c = fat_get(x, c);
            if (!valid_clus(x, c)) return false;
        }
    }
    if (!valid_clus(x, c)) return false;
    uint32_t byte = (idx % per) * 32;
    lba = clus_lba(x, c) + byte / x->bps;
    off = byte % x->bps;
    return true;
}

bool entry_read(X* x, const Chain& ch, uint32_t idx, uint8_t e[32]) {
    uint64_t lba;
    uint32_t off;
    if (!entry_pos(x, ch, idx, lba, off)) return false;
    if (x->secbuf_lba != lba) {
        if (!blk::read(x->vol, lba, 1, x->secbuf)) return false;
        x->secbuf_lba = lba;
    }
    memcpy(e, x->secbuf + off, 32);
    return true;
}

bool entry_write(X* x, const Chain& ch, uint32_t idx, const uint8_t e[32]) {
    uint64_t lba;
    uint32_t off;
    if (!entry_pos(x, ch, idx, lba, off)) return false;
    if (x->secbuf_lba != lba) {
        if (!blk::read(x->vol, lba, 1, x->secbuf)) return false;
        x->secbuf_lba = lba;
    }
    memcpy(x->secbuf + off, e, 32);
    return blk::write(x->vol, lba, 1, x->secbuf);
}

uint16_t set_checksum(const uint8_t* set, int count) {
    uint16_t s = 0;
    for (int i = 0; i < count * 32; i++) {
        if (i == 2 || i == 3) continue;
        s = uint16_t(((s & 1) ? 0x8000 : 0) + (s >> 1) + set[i]);
    }
    return s;
}

uint16_t upcase(uint16_t c) {
    if (c >= 'a' && c <= 'z') return uint16_t(c - 32);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return uint16_t(c - 32);
    if (c >= 0x430 && c <= 0x44F) return uint16_t(c - 32);
    if (c >= 0x450 && c <= 0x45F) return uint16_t(c - 80);
    return c;
}

uint16_t name_hash(const uint16_t* n, int len) {
    uint16_t h = 0;
    for (int i = 0; i < len; i++) {
        uint16_t c = upcase(n[i]);
        h = uint16_t(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c & 0xFF));
        h = uint16_t(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c >> 8));
    }
    return h;
}

size_t utf16_to_utf8(const uint16_t* in, int n, char* out, size_t cap) {
    size_t o = 0;
    for (int i = 0; i < n && in[i]; i++) {
        uint32_t c = in[i];
        if (c >= 0xD800 && c <= 0xDFFF) c = '?';
        if (c < 0x80 && o + 1 < cap) out[o++] = char(c);
        else if (c < 0x800 && o + 2 < cap) { out[o++] = char(0xC0 | (c >> 6)); out[o++] = char(0x80 | (c & 0x3F)); }
        else if (o + 3 < cap) {
            out[o++] = char(0xE0 | (c >> 12));
            out[o++] = char(0x80 | ((c >> 6) & 0x3F));
            out[o++] = char(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
    return o;
}

int utf8_to_utf16(const char* s, uint16_t* out, int cap) {
    int n = 0;
    while (*s && n < cap) {
        uint8_t b = uint8_t(*s++);
        uint32_t c = b;
        if (b >= 0xF0) { c = '_'; while ((uint8_t(*s) & 0xC0) == 0x80) s++; }
        else if (b >= 0xE0) { c = (b & 0x0F) << 12; c |= (uint8_t(*s++) & 0x3F) << 6; c |= uint8_t(*s++) & 0x3F; }
        else if (b >= 0xC0) { c = (b & 0x1F) << 6; c |= uint8_t(*s++) & 0x3F; }
        out[n++] = uint16_t(c);
    }
    return n;
}

void timestamp(uint8_t* e85) {
    rtc::Time t;
    if (!rtc::read(t) || t.year < 1980) t = {2026, 1, 1, 0, 0, 0};
    uint32_t ts = (uint32_t(t.year - 1980) << 25) | (uint32_t(t.month) << 21) | (uint32_t(t.day) << 16) |
                  (uint32_t(t.hour) << 11) | (uint32_t(t.min) << 5) | (t.sec / 2);
    memcpy(e85 + 8, &ts, 4);   // create
    memcpy(e85 + 12, &ts, 4);  // modified
    memcpy(e85 + 16, &ts, 4);  // accessed
    e85[20] = e85[21] = 0;
    e85[22] = e85[23] = e85[24] = 0;
}

// Reads an entry set (0x85 + secondaries) starting at idx into buf.
bool read_set(X* x, const Chain& ch, uint32_t idx, uint8_t* buf, int& count) {
    if (!entry_read(x, ch, idx, buf) || buf[0] != 0x85) return false;
    count = buf[1] + 1;
    if (count < 3 || count > 19) return false;
    for (int i = 1; i < count; i++)
        if (!entry_read(x, ch, idx + uint32_t(i), buf + i * 32)) return false;
    return true;
}

bool write_set(X* x, const Chain& ch, uint32_t idx, uint8_t* buf, int count) {
    uint16_t sum = set_checksum(buf, count);
    buf[2] = uint8_t(sum);
    buf[3] = uint8_t(sum >> 8);
    for (int i = 0; i < count; i++)
        if (!entry_write(x, ch, idx + uint32_t(i), buf + i * 32)) return false;
    return true;
}

// ---------------------------------------------------------------- VFS ops

bool load_dir(fs::Node* dir) {
    X* x = M(dir);
    Chain ch = dir_chain(dir);
    if (!valid_clus(x, ch.first)) return true;
    uint8_t set[19 * 32];
    for (uint32_t i = 0;; i++) {
        uint8_t e[32];
        if (!entry_read(x, ch, i, e)) return true;
        if (e[0] == 0) return true;
        if (e[0] != 0x85) continue;
        int count;
        if (!read_set(x, ch, i, set, count)) continue;
        const uint8_t* st = set + 32;
        if (st[0] != 0xC0) continue;
        int namelen = st[3];
        uint16_t name16[256];
        int got = 0;
        for (int k = 2; k < count && got < namelen; k++) {
            const uint8_t* ne = set + k * 32;
            if (ne[0] != 0xC1) break;
            for (int j = 0; j < 15 && got < namelen; j++) name16[got++] = uint16_t(ne[2 + 2 * j] | (ne[3 + 2 * j] << 8));
        }
        char name[fs::NAME_MAX * 3 + 4];
        utf16_to_utf8(name16, got, name, sizeof(name));
        if (strlen(name) > fs::NAME_MAX) name[fs::NAME_MAX] = 0;
        uint16_t attr = uint16_t(set[4] | (set[5] << 8));
        bool is_dir = attr & 0x10;
        fs::Node* n = fs::attach(dir, name, is_dir);
        if (!n) return false;
        uint32_t first;
        uint64_t valid, dlen;
        memcpy(&first, st + 20, 4);
        memcpy(&valid, st + 8, 8);
        memcpy(&dlen, st + 24, 8);
        bool contig = st[1] & 0x02;
        n->fs_a = first;
        n->fs_b = i;
        n->fs_c = uint64_t(count - 1) | (uint64_t(contig) << 8) | (dlen << 16);
        if (!is_dir) n->size = size_t(valid);
        i += uint32_t(count - 1);
    }
}

bool load_file(fs::Node* n) {
    X* x = M(n);
    Chain ch = dir_chain(n);  // same field layout for files
    size_t size = n->size;
    auto* buf = static_cast<uint8_t*>(kmalloc(size ? size : 1));
    if (!buf) return false;
    size_t done = 0;
    uint32_t c = ch.first;
    for (uint32_t i = 0; done < size && valid_clus(x, c); i++) {
        if (!read_clus(x, c, x->cbuf)) {
            kfree(buf);
            return false;
        }
        size_t chunk = size - done < x->cbytes ? size - done : x->cbytes;
        memcpy(buf + done, x->cbuf, chunk);
        done += chunk;
        c = chain_next(x, ch, c, i);
    }
    n->size = 0;
    bool ok = done == size && fs::set_data(n, buf, size);
    kfree(buf);
    return ok;
}

// Converts a contiguous directory to a FAT chain and appends one zeroed
// cluster. Updates the directory's stream entry in its parent.
bool extend_dir(X* x, fs::Node* dir) {
    Chain ch = dir_chain(dir);
    bool is_root = dir->parent && dir->parent->mnt != dir->mnt;  // mount point
    uint32_t last = ch.first;
    if (ch.contig) {
        uint32_t n = uint32_t((ch.len + x->cbytes - 1) / x->cbytes);
        for (uint32_t i = 0; i + 1 < n; i++) fat_set(x, ch.first + i, ch.first + i + 1);
        last = ch.first + (n ? n - 1 : 0);
        fat_set(x, last, 0xFFFFFFFF);
    } else {
        for (uint32_t c = ch.first, i = 0; valid_clus(x, c) && i < x->nclusters; c = fat_get(x, c), i++) last = c;
    }
    uint32_t nc = alloc_one(x);
    if (!nc) return false;
    memset(x->cbuf, 0, x->cbytes);
    if (!write_clus(x, nc, x->cbuf)) return false;
    fat_set(x, last, nc);
    uint64_t newlen = (ch.len ? ch.len : 0) + x->cbytes;
    dir->fs_c = (dir->fs_c & 0xFF) | (newlen << 16);  // no longer contiguous
    if (!is_root) {
        uint8_t set[19 * 32];
        int count;
        Chain pch = dir_chain(dir->parent);
        if (!read_set(x, pch, uint32_t(dir->fs_b), set, count)) return false;
        uint8_t* st = set + 32;
        st[1] = uint8_t(st[1] & ~0x02);
        memcpy(st + 8, &newlen, 8);
        memcpy(st + 24, &newlen, 8);
        if (!write_set(x, pch, uint32_t(dir->fs_b), set, count)) return false;
    }
    return fat_flush(x);
}

fs::Error create(fs::Node* dir, fs::Node* n) {
    X* x = M(dir);
    if (x->base.ro) return fs::ReadOnly;
    uint16_t name16[256];
    int nl = utf8_to_utf16(n->name, name16, 255);
    int nname = (nl + 14) / 15;
    int need = 2 + nname;

    uint32_t start = 0;
    int run = 0;
    for (uint32_t i = 0;; i++) {
        uint8_t e[32];
        if (!entry_read(x, dir_chain(dir), i, e)) {
            if (!extend_dir(x, dir)) return fs::NoSpace;
            if (!entry_read(x, dir_chain(dir), i, e)) return fs::IoError;
        }
        if (!(e[0] & 0x80)) {  // unused or deleted
            if (run == 0) start = i;
            if (++run == need) break;
        } else {
            run = 0;
        }
    }

    uint32_t cl = 0;
    uint64_t len = 0;
    if (n->dir) {
        cl = alloc_one(x);
        if (!cl) return fs::NoSpace;
        memset(x->cbuf, 0, x->cbytes);
        if (!write_clus(x, cl, x->cbuf)) return fs::IoError;
        len = x->cbytes;
    }
    uint8_t set[19 * 32] = {};
    uint8_t* fe = set;
    fe[0] = 0x85;
    fe[1] = uint8_t(need - 1);
    fe[4] = n->dir ? 0x10 : 0x20;
    timestamp(fe);
    uint8_t* st = set + 32;
    st[0] = 0xC0;
    st[1] = 0x01;  // allocation possible, FAT chain
    st[3] = uint8_t(nl);
    uint16_t h = name_hash(name16, nl);
    st[4] = uint8_t(h);
    st[5] = uint8_t(h >> 8);
    memcpy(st + 8, &len, 8);
    memcpy(st + 20, &cl, 4);
    memcpy(st + 24, &len, 8);
    for (int k = 0; k < nname; k++) {
        uint8_t* ne = set + (2 + k) * 32;
        ne[0] = 0xC1;
        for (int j = 0; j < 15; j++) {
            int ci = k * 15 + j;
            uint16_t c = ci < nl ? name16[ci] : 0;
            ne[2 + 2 * j] = uint8_t(c);
            ne[3 + 2 * j] = uint8_t(c >> 8);
        }
    }
    if (!write_set(x, dir_chain(dir), start, set, need)) return fs::IoError;
    n->fs_a = cl;
    n->fs_b = start;
    n->fs_c = uint64_t(need - 1) | (len << 16);
    return fat_flush(x) ? fs::Ok : fs::IoError;
}

fs::Error write(fs::Node* n, const void* data, size_t len) {
    X* x = M(n);
    if (x->base.ro) return fs::ReadOnly;
    Chain pch = dir_chain(n->parent);
    uint8_t set[19 * 32];
    int count;
    if (!read_set(x, pch, uint32_t(n->fs_b), set, count)) return fs::IoError;
    uint8_t* st = set + 32;
    uint32_t old_first;
    uint64_t old_len;
    memcpy(&old_first, st + 20, 4);
    memcpy(&old_len, st + 24, 8);
    Chain old{old_first, bool(st[1] & 0x02), old_len};

    uint32_t nclus = uint32_t((len + x->cbytes - 1) / x->cbytes);
    uint32_t first = nclus ? alloc_chain(x, nclus) : 0;
    if (nclus && !first) {
        fat_flush(x);
        return fs::NoSpace;
    }
    auto* p = static_cast<const uint8_t*>(data);
    size_t done = 0;
    for (uint32_t c = first; done < len; c = fat_get(x, c)) {
        size_t chunk = len - done < x->cbytes ? len - done : x->cbytes;
        memcpy(x->cbuf, p + done, chunk);
        if (chunk < x->cbytes) memset(x->cbuf + chunk, 0, x->cbytes - chunk);
        if (!write_clus(x, c, x->cbuf)) {
            free_chain(x, {first, false, 0});
            fat_flush(x);
            return fs::IoError;
        }
        done += chunk;
    }
    uint64_t l64 = len;
    st[1] = 0x01;
    memcpy(st + 8, &l64, 8);
    memcpy(st + 20, &first, 4);
    memcpy(st + 24, &l64, 8);
    timestamp(set);
    set[4] |= 0x20;
    if (!write_set(x, pch, uint32_t(n->fs_b), set, count)) return fs::IoError;
    if (valid_clus(x, old.first)) free_chain(x, old);
    n->fs_a = first;
    n->fs_c = (n->fs_c & 0xFF) | (l64 << 16);
    return fat_flush(x) ? fs::Ok : fs::IoError;
}

fs::Error remove(fs::Node* n) {
    X* x = M(n);
    if (x->base.ro) return fs::ReadOnly;
    Chain pch = dir_chain(n->parent);
    uint8_t set[19 * 32];
    int count;
    if (!read_set(x, pch, uint32_t(n->fs_b), set, count)) return fs::IoError;
    uint8_t* st = set + 32;
    uint32_t first;
    uint64_t dlen;
    memcpy(&first, st + 20, 4);
    memcpy(&dlen, st + 24, 8);
    for (int i = 0; i < count; i++) {
        uint8_t* e = set + i * 32;
        e[0] = uint8_t(e[0] & 0x7F);
        if (!entry_write(x, pch, uint32_t(n->fs_b) + uint32_t(i), e)) return fs::IoError;
    }
    if (valid_clus(x, first)) free_chain(x, {first, bool(st[1] & 0x02), dlen});
    return fat_flush(x) ? fs::Ok : fs::IoError;
}

const fs::Ops kOps = {load_dir, load_file, create, write, remove};

}  // namespace

bool probe_mount(const blk::Volume& v, const char* mname, bool all_rw, char* info, size_t cap) {
    uint32_t ss = v.dev->sector_size;
    auto* bs = static_cast<uint8_t*>(kmalloc(ss));
    if (!bs) return false;
    bool ok = blk::read(v, 0, 1, bs) && memcmp(bs + 3, "EXFAT   ", 8) == 0;
    if (!ok) {
        kfree(bs);
        return false;
    }
    auto* x = static_cast<X*>(kzalloc(sizeof(X)));
    x->vol = v;
    x->bps = 1u << bs[108];
    x->spc = 1u << bs[109];
    x->cbytes = x->bps * x->spc;
    memcpy(&x->fat_off, bs + 80, 4);
    memcpy(&x->heap_off, bs + 88, 4);
    memcpy(&x->nclusters, bs + 92, 4);
    memcpy(&x->root, bs + 96, 4);
    kfree(bs);
    if (x->bps != ss || x->cbytes > (32u << 20)) return false;
    x->fatbuf = static_cast<uint8_t*>(kmalloc(x->bps));
    x->fatbuf_sec = 0xFFFFFFFF;
    x->secbuf = static_cast<uint8_t*>(kmalloc(x->bps));
    x->secbuf_lba = ~0ull;
    x->cbuf = static_cast<uint8_t*>(kmalloc(x->cbytes));
    x->hint = 2;
    if (!x->fatbuf || !x->secbuf || !x->cbuf) return false;

    // Root directory: allocation bitmap + label.
    Chain root{x->root, false, 0};
    uint32_t bm_first = 0;
    uint64_t bm_len = 0;
    for (uint32_t i = 0; i < 4096; i++) {
        uint8_t e[32];
        if (!entry_read(x, root, i, e) || e[0] == 0) break;
        if (e[0] == 0x81 && !(e[1] & 1)) {
            memcpy(&bm_first, e + 20, 4);
            memcpy(&bm_len, e + 24, 8);
        } else if (e[0] == 0x83) {
            uint16_t l16[11];
            int n = e[1] > 11 ? 11 : e[1];
            for (int k = 0; k < n; k++) l16[k] = uint16_t(e[2 + 2 * k] | (e[3 + 2 * k] << 8));
            utf16_to_utf8(l16, n, x->label, sizeof(x->label));
        }
    }
    if (!valid_clus(x, bm_first) || bm_len < (x->nclusters + 7) / 8) return false;
    x->bm_nclusters = uint32_t((bm_len + x->cbytes - 1) / x->cbytes);
    x->bm_clusters = static_cast<uint32_t*>(kmalloc(sizeof(uint32_t) * x->bm_nclusters));
    x->bitmap = static_cast<uint8_t*>(kmalloc(size_t(x->bm_nclusters) * x->cbytes));
    if (!x->bm_clusters || !x->bitmap) return false;
    uint32_t c = bm_first;
    for (uint32_t i = 0; i < x->bm_nclusters; i++) {
        if (!valid_clus(x, c) || !read_clus(x, c, x->bitmap + size_t(i) * x->cbytes)) return false;
        x->bm_clusters[i] = c;
        uint32_t nx = fat_get(x, c);
        c = (nx == 0) ? c + 1 : nx;  // some formatters leave the bitmap unchained (contiguous)
    }
    x->base.ro = !all_rw;
    x->base.can_write = true;
    if (!fs::mount(mname, &kOps, x, x->root, 0, 0)) return false;
    ksnprintf(info, cap, "exFAT \"%s\", %lu MiB, cluster %u B, %s", x->label,
              uint64_t(x->nclusters) * x->cbytes >> 20, x->cbytes, x->base.ro ? "read-only" : "read-write");
    return true;
}

}  // namespace exfat
