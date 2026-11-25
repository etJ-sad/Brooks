// SPDX-License-Identifier: GPL-2.0-only
#include "fs/fat.h"
#include "block/block.h"
#include "drivers/rtc.h"
#include "fs/vfs.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"

namespace fat {
namespace {

constexpr uint32_t EOC = 0x0FFFFFF8;

struct Fat {
    blk::Volume vol;
    uint32_t bps, spc, rsvd, nfats, fatsz, root, data_start, nclusters, cbytes, fsinfo;
    bool ro;
    uint32_t hint;
    uint8_t* fatbuf;
    uint32_t fatbuf_sec;
    bool fat_dirty;
    uint8_t* secbuf;
    uint8_t* cbuf;
    char label[12];
};

Fat* F(fs::Node* n) { return static_cast<Fat*>(n->mnt); }

// ---------------------------------------------------------------- FAT table

bool fat_flush(Fat* f) {
    if (!f->fat_dirty) return true;
    for (uint32_t i = 0; i < f->nfats; i++)
        if (!blk::write(f->vol, f->rsvd + i * f->fatsz + f->fatbuf_sec, 1, f->fatbuf)) return false;
    f->fat_dirty = false;
    return true;
}

bool fat_load(Fat* f, uint32_t sec) {
    if (f->fatbuf_sec == sec) return true;
    if (!fat_flush(f)) return false;
    if (!blk::read(f->vol, f->rsvd + sec, 1, f->fatbuf)) {
        f->fatbuf_sec = 0xFFFFFFFF;
        return false;
    }
    f->fatbuf_sec = sec;
    return true;
}

uint32_t fat_get(Fat* f, uint32_t c) {
    if (!fat_load(f, (c * 4) / f->bps)) return 0x0FFFFFFF;
    uint32_t v;
    memcpy(&v, f->fatbuf + (c * 4) % f->bps, 4);
    return v & 0x0FFFFFFF;
}

bool fat_set(Fat* f, uint32_t c, uint32_t val) {
    if (!fat_load(f, (c * 4) / f->bps)) return false;
    uint32_t old;
    uint8_t* p = f->fatbuf + (c * 4) % f->bps;
    memcpy(&old, p, 4);
    val = (old & 0xF0000000) | (val & 0x0FFFFFFF);
    memcpy(p, &val, 4);
    f->fat_dirty = true;
    return true;
}

uint64_t clus_lba(Fat* f, uint32_t c) { return f->data_start + uint64_t(c - 2) * f->spc; }
bool read_clus(Fat* f, uint32_t c, void* buf) { return blk::read(f->vol, clus_lba(f, c), f->spc, buf); }
bool write_clus(Fat* f, uint32_t c, const void* buf) { return blk::write(f->vol, clus_lba(f, c), f->spc, buf); }

uint32_t alloc_one(Fat* f) {
    uint32_t max = f->nclusters + 2;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t start = pass ? 2 : (f->hint >= 2 && f->hint < max ? f->hint : 2);
        uint32_t end = pass ? f->hint : max;
        for (uint32_t c = start; c < end; c++) {
            if (fat_get(f, c) == 0) {
                if (!fat_set(f, c, 0x0FFFFFFF)) return 0;
                f->hint = c + 1;
                return c;
            }
        }
    }
    return 0;
}

void free_chain(Fat* f, uint32_t c) {
    for (uint32_t guard = 0; c >= 2 && c < EOC && guard < f->nclusters + 2; guard++) {
        uint32_t next = fat_get(f, c);
        fat_set(f, c, 0);
        if (c < f->hint) f->hint = c;
        c = next;
    }
}

// Allocates a chain of n clusters; returns first cluster or 0.
uint32_t alloc_chain(Fat* f, uint32_t n) {
    uint32_t first = 0, prev = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = alloc_one(f);
        if (!c) {
            free_chain(f, first);
            return 0;
        }
        if (prev) fat_set(f, prev, c);
        else first = c;
        prev = c;
    }
    return first;
}

// ---------------------------------------------------------------- directory entries

// Locates entry `idx` of the directory starting at cluster `dc`: returns the
// volume sector and byte offset. Fails past the end of the chain.
bool entry_pos(Fat* f, uint32_t dc, uint32_t idx, uint64_t& sec, uint32_t& off) {
    uint32_t per = f->cbytes / 32;
    uint32_t c = dc;
    for (uint32_t i = 0; i < idx / per; i++) {
        c = fat_get(f, c);
        if (c < 2 || c >= EOC) return false;
    }
    uint32_t byte = (idx % per) * 32;
    sec = clus_lba(f, c) + byte / f->bps;
    off = byte % f->bps;
    return true;
}

bool entry_read(Fat* f, uint32_t dc, uint32_t idx, uint8_t e[32]) {
    uint64_t sec;
    uint32_t off;
    if (!entry_pos(f, dc, idx, sec, off) || !blk::read(f->vol, sec, 1, f->secbuf)) return false;
    memcpy(e, f->secbuf + off, 32);
    return true;
}

bool entry_write(Fat* f, uint32_t dc, uint32_t idx, const uint8_t e[32]) {
    uint64_t sec;
    uint32_t off;
    if (!entry_pos(f, dc, idx, sec, off) || !blk::read(f->vol, sec, 1, f->secbuf)) return false;
    memcpy(f->secbuf + off, e, 32);
    return blk::write(f->vol, sec, 1, f->secbuf);
}

uint8_t lfn_checksum(const uint8_t* n11) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = uint8_t(((s & 1) << 7) + (s >> 1) + n11[i]);
    return s;
}

void dos_time(uint16_t& date, uint16_t& time) {
    rtc::Time t;
    if (!rtc::read(t) || t.year < 1980) t = {2026, 1, 1, 0, 0, 0};
    date = uint16_t(((t.year - 1980) << 9) | (t.month << 5) | t.day);
    time = uint16_t((t.hour << 11) | (t.min << 5) | (t.sec / 2));
}

size_t utf16_to_utf8(const uint16_t* in, int n, char* out, size_t cap) {
    size_t o = 0;
    for (int i = 0; i < n && in[i] && in[i] != 0xFFFF; i++) {
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

// ---------------------------------------------------------------- VFS ops

bool load_dir(fs::Node* dir) {
    Fat* f = F(dir);
    uint32_t dc = uint32_t(dir->fs_a);
    if (dc < 2) return true;  // empty directory without clusters
    uint16_t lfn[260];
    int lfn_total = 0, lfn_seen = 0;
    uint8_t lfn_sum = 0;
    bool lfn_ok = false;
    uint32_t per = f->cbytes / 32, idx = 0;
    char name[fs::NAME_MAX * 3 + 4];
    for (uint32_t c = dc, guard = 0; c >= 2 && c < EOC && guard < f->nclusters; c = fat_get(f, c), guard++) {
        if (!read_clus(f, c, f->cbuf)) return false;
        for (uint32_t i = 0; i < per; i++, idx++) {
            const uint8_t* e = f->cbuf + i * 32;
            if (e[0] == 0) return true;
            if (e[0] == 0xE5) { lfn_ok = false; continue; }
            if (e[11] == 0x0F) {
                int seq = e[0] & 0x3F;
                if (e[0] & 0x40) {
                    lfn_total = seq;
                    lfn_seen = 0;
                    lfn_sum = e[13];
                    lfn_ok = seq >= 1 && seq <= 20;
                    for (int k = 0; k < 260; k++) lfn[k] = 0;
                }
                if (!lfn_ok || seq < 1 || seq > lfn_total || e[13] != lfn_sum) { lfn_ok = false; continue; }
                int base = (seq - 1) * 13;
                static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                for (int k = 0; k < 13 && base + k < 260; k++) lfn[base + k] = uint16_t(e[pos[k]] | (e[pos[k] + 1] << 8));
                lfn_seen++;
                continue;
            }
            if (e[11] & 0x08) { lfn_ok = false; continue; }  // volume label
            bool dot = e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '));
            if (dot) { lfn_ok = false; continue; }
            int lfn_count = 0;
            if (lfn_ok && lfn_seen == lfn_total && lfn_checksum(e) == lfn_sum) {
                utf16_to_utf8(lfn, 260, name, sizeof(name));
                lfn_count = lfn_total;
            } else {
                // 8.3 name, lower-case flags from NT byte
                int o = 0;
                for (int k = 0; k < 8 && e[k] != ' '; k++) {
                    char ch = char(k == 0 && e[0] == 0x05 ? 0xE5 : e[k]);
                    if ((e[12] & 0x08) && ch >= 'A' && ch <= 'Z') ch = char(ch + 32);
                    name[o++] = ch;
                }
                if (e[8] != ' ') {
                    name[o++] = '.';
                    for (int k = 8; k < 11 && e[k] != ' '; k++) {
                        char ch = char(e[k]);
                        if ((e[12] & 0x10) && ch >= 'A' && ch <= 'Z') ch = char(ch + 32);
                        name[o++] = ch;
                    }
                }
                name[o] = 0;
            }
            lfn_ok = false;
            if (strlen(name) > fs::NAME_MAX) name[fs::NAME_MAX] = 0;
            fs::Node* n = fs::attach(dir, name, e[11] & 0x10);
            if (!n) return false;
            n->fs_a = uint32_t(e[26] | (e[27] << 8)) | (uint32_t(e[20] | (e[21] << 8)) << 16);
            n->fs_b = idx;
            n->fs_c = uint64_t(lfn_count);
            if (!n->dir) memcpy(&n->size, e + 28, 4);
        }
    }
    return true;
}

bool load_file(fs::Node* n) {
    Fat* f = F(n);
    size_t size = n->size;
    auto* buf = static_cast<uint8_t*>(kmalloc(size ? size : 1));
    if (!buf) return false;
    size_t done = 0;
    for (uint32_t c = uint32_t(n->fs_a), guard = 0; done < size && c >= 2 && c < EOC && guard < f->nclusters;
         c = fat_get(f, c), guard++) {
        if (!read_clus(f, c, f->cbuf)) {
            kfree(buf);
            return false;
        }
        size_t chunk = size - done < f->cbytes ? size - done : f->cbytes;
        memcpy(buf + done, f->cbuf, chunk);
        done += chunk;
    }
    n->size = 0;  // set_data accounts the new size
    bool ok = done == size && fs::set_data(n, buf, size);
    kfree(buf);
    return ok;
}

bool short_exists(Fat* f, uint32_t dc, const uint8_t* n11) {
    uint8_t e[32];
    for (uint32_t i = 0;; i++) {
        if (!entry_read(f, dc, i, e) || e[0] == 0) return false;
        if (e[0] != 0xE5 && e[11] != 0x0F && memcmp(e, n11, 11) == 0) return true;
    }
}

// Builds an 8.3 name. Returns true if the long name is needed as well.
bool make_short(Fat* f, uint32_t dc, const char* name, uint8_t out[11]) {
    memset(out, ' ', 11);
    const char* dot = nullptr;
    for (const char* p = name; *p; p++)
        if (*p == '.') dot = p;
    if (dot == name) dot = nullptr;
    auto valid = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~' || c == '!' ||
               c == '#' || c == '$' || c == '%' || c == '&' || c == '(' || c == ')' || c == '@' || c == '^' ||
               c == '{' || c == '}' || c == '\'' || c == '`';
    };
    bool lossy = false;
    int bl = 0;
    for (const char* p = name; *p && p != dot && bl < 8; p++) {
        char c = *p;
        if (c == ' ' || c == '.') { lossy = true; continue; }
        if (c >= 'a' && c <= 'z') { c = char(c - 32); lossy = true; }
        if (uint8_t(c) >= 0x80) { while ((uint8_t(p[1]) & 0xC0) == 0x80) p++; c = '_'; lossy = true; }
        else if (!valid(c)) { c = '_'; lossy = true; }
        out[bl++] = uint8_t(c);
    }
    size_t base_len = dot ? size_t(dot - name) : strlen(name);
    if (base_len > 8) lossy = true;
    if (bl == 0) { out[0] = '_'; bl = 1; lossy = true; }
    if (dot) {
        int el = 0;
        for (const char* p = dot + 1; *p && el < 3; p++) {
            char c = *p;
            if (c >= 'a' && c <= 'z') { c = char(c - 32); lossy = true; }
            if (uint8_t(c) >= 0x80) { while ((uint8_t(p[1]) & 0xC0) == 0x80) p++; c = '_'; lossy = true; }
            else if (!valid(c)) { c = '_'; lossy = true; }
            out[8 + el++] = uint8_t(c);
        }
        if (strlen(dot + 1) > 3) lossy = true;
    }
    if (!lossy && !short_exists(f, dc, out)) return false;
    // numeric tail ~N
    for (int n = 1; n < 1000000; n++) {
        char tail[8];
        int tl = int(ksnprintf(tail, sizeof(tail), "~%d", n));
        int keep = bl < 8 - tl ? bl : 8 - tl;
        uint8_t cand[11];
        memcpy(cand, out, 11);
        for (int k = keep; k < 8; k++) cand[k] = ' ';
        memcpy(cand + keep, tail, size_t(tl));
        if (!short_exists(f, dc, cand)) {
            memcpy(out, cand, 11);
            return true;
        }
    }
    return true;
}

fs::Error flush_err(Fat* f) { return fat_flush(f) ? fs::Ok : fs::IoError; }

fs::Error create(fs::Node* dir, fs::Node* n) {
    Fat* f = F(dir);
    if (f->ro) return fs::ReadOnly;
    uint32_t dc = uint32_t(dir->fs_a);
    if (dc < 2) return fs::IoError;
    uint8_t short11[11];
    bool need_lfn = make_short(f, dc, n->name, short11);
    uint16_t u16[260];
    int ulen = utf8_to_utf16(n->name, u16, 255);
    int nlfn = need_lfn ? (ulen + 12) / 13 : 0;
    int need = nlfn + 1;

    // Find `need` consecutive free entries, extending the directory if needed.
    uint32_t start = 0;
    int run = 0;
    uint8_t e[32];
    for (uint32_t i = 0;; i++) {
        if (!entry_read(f, dc, i, e)) {
            // end of chain: append a zeroed cluster
            uint32_t last = dc;
            for (uint32_t c = dc; c >= 2 && c < EOC; c = fat_get(f, c)) last = c;
            uint32_t nc = alloc_one(f);
            if (!nc) return fs::NoSpace;
            memset(f->cbuf, 0, f->cbytes);
            if (!write_clus(f, nc, f->cbuf)) return fs::IoError;
            fat_set(f, last, nc);
            if (!entry_read(f, dc, i, e)) return fs::IoError;
        }
        if (e[0] == 0 || e[0] == 0xE5) {
            if (run == 0) start = i;
            if (++run == need) break;
        } else {
            run = 0;
        }
    }

    uint32_t cl = 0;
    uint16_t date, time;
    dos_time(date, time);
    if (n->dir) {
        cl = alloc_one(f);
        if (!cl) return fs::NoSpace;
        memset(f->cbuf, 0, f->cbytes);
        uint32_t parent = (dc == f->root) ? 0 : dc;
        auto dot_entry = [&](uint8_t* d, const char* nm, uint32_t c) {
            memset(d, ' ', 11);
            memcpy(d, nm, strlen(nm));
            d[11] = 0x10;
            d[20] = uint8_t(c >> 16); d[21] = uint8_t(c >> 24);
            d[26] = uint8_t(c); d[27] = uint8_t(c >> 8);
            d[22] = uint8_t(time); d[23] = uint8_t(time >> 8);
            d[24] = uint8_t(date); d[25] = uint8_t(date >> 8);
        };
        dot_entry(f->cbuf, ".", cl);
        dot_entry(f->cbuf + 32, "..", parent);
        if (!write_clus(f, cl, f->cbuf)) return fs::IoError;
    }

    uint8_t sum = lfn_checksum(short11);
    for (int k = 0; k < nlfn; k++) {
        int seq = nlfn - k;  // physical order: last fragment first
        uint8_t l[32] = {};
        l[0] = uint8_t(seq | (k == 0 ? 0x40 : 0));
        l[11] = 0x0F;
        l[13] = sum;
        static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (int j = 0; j < 13; j++) {
            int ci = (seq - 1) * 13 + j;
            uint16_t ch = ci < ulen ? u16[ci] : (ci == ulen ? 0 : 0xFFFF);
            l[pos[j]] = uint8_t(ch);
            l[pos[j] + 1] = uint8_t(ch >> 8);
        }
        if (!entry_write(f, dc, start + uint32_t(k), l)) return fs::IoError;
    }
    uint8_t s[32] = {};
    memcpy(s, short11, 11);
    s[11] = n->dir ? 0x10 : 0x20;
    s[14] = uint8_t(time); s[15] = uint8_t(time >> 8);
    s[16] = uint8_t(date); s[17] = uint8_t(date >> 8);
    s[18] = uint8_t(date); s[19] = uint8_t(date >> 8);
    s[20] = uint8_t(cl >> 16); s[21] = uint8_t(cl >> 24);
    s[22] = uint8_t(time); s[23] = uint8_t(time >> 8);
    s[24] = uint8_t(date); s[25] = uint8_t(date >> 8);
    s[26] = uint8_t(cl); s[27] = uint8_t(cl >> 8);
    uint32_t sidx = start + uint32_t(nlfn);
    if (!entry_write(f, dc, sidx, s)) return fs::IoError;
    n->fs_a = cl;
    n->fs_b = sidx;
    n->fs_c = uint64_t(nlfn);
    return flush_err(f);
}

fs::Error write(fs::Node* n, const void* data, size_t len) {
    Fat* f = F(n);
    if (f->ro) return fs::ReadOnly;
    if (len > 0xFFFFFFFFull) return fs::NoSpace;
    uint32_t dc = uint32_t(n->parent->fs_a);
    uint32_t count = uint32_t((len + f->cbytes - 1) / f->cbytes);
    uint32_t first = count ? alloc_chain(f, count) : 0;
    if (count && !first) {
        fat_flush(f);
        return fs::NoSpace;
    }
    auto* p = static_cast<const uint8_t*>(data);
    size_t done = 0;
    for (uint32_t c = first; done < len; c = fat_get(f, c)) {
        size_t chunk = len - done < f->cbytes ? len - done : f->cbytes;
        memcpy(f->cbuf, p + done, chunk);
        if (chunk < f->cbytes) memset(f->cbuf + chunk, 0, f->cbytes - chunk);
        if (!write_clus(f, c, f->cbuf)) {
            free_chain(f, first);
            fat_flush(f);
            return fs::IoError;
        }
        done += chunk;
    }
    uint8_t e[32];
    if (!entry_read(f, dc, uint32_t(n->fs_b), e)) return fs::IoError;
    uint32_t old = uint32_t(e[26] | (e[27] << 8)) | (uint32_t(e[20] | (e[21] << 8)) << 16);
    uint16_t date, time;
    dos_time(date, time);
    e[20] = uint8_t(first >> 16); e[21] = uint8_t(first >> 24);
    e[26] = uint8_t(first); e[27] = uint8_t(first >> 8);
    uint32_t sz = uint32_t(len);
    memcpy(e + 28, &sz, 4);
    e[22] = uint8_t(time); e[23] = uint8_t(time >> 8);
    e[24] = uint8_t(date); e[25] = uint8_t(date >> 8);
    e[11] |= 0x20;  // archive
    if (!entry_write(f, dc, uint32_t(n->fs_b), e)) return fs::IoError;
    free_chain(f, old);
    n->fs_a = first;
    return flush_err(f);
}

fs::Error remove(fs::Node* n) {
    Fat* f = F(n);
    if (f->ro) return fs::ReadOnly;
    uint32_t dc = uint32_t(n->parent->fs_a);
    uint8_t e[32];
    for (uint32_t i = uint32_t(n->fs_b - n->fs_c); i <= uint32_t(n->fs_b); i++) {
        if (!entry_read(f, dc, i, e)) return fs::IoError;
        e[0] = 0xE5;
        if (!entry_write(f, dc, i, e)) return fs::IoError;
    }
    free_chain(f, uint32_t(n->fs_a));
    return flush_err(f);
}

const fs::Ops kOps = {load_dir, load_file, create, write, remove};

Fat* try_open(const blk::Volume& v) {
    uint32_t ss = v.dev->sector_size;
    auto* bs = static_cast<uint8_t*>(kmalloc(ss));
    if (!bs) return nullptr;
    Fat* result = nullptr;
    if (blk::read(v, 0, 1, bs) && bs[510] == 0x55 && bs[511] == 0xAA) {
        uint32_t bps = uint32_t(bs[11] | (bs[12] << 8));
        uint32_t spc = bs[13];
        uint32_t rsvd = uint32_t(bs[14] | (bs[15] << 8));
        uint32_t nfats = bs[16];
        uint32_t fatsz16 = uint32_t(bs[22] | (bs[23] << 8));
        uint32_t fatsz32, total16 = uint32_t(bs[19] | (bs[20] << 8)), total32, root;
        memcpy(&fatsz32, bs + 36, 4);
        memcpy(&total32, bs + 32, 4);
        memcpy(&root, bs + 44, 4);
        uint32_t total = total16 ? total16 : total32;
        bool pow2 = spc && !(spc & (spc - 1));
        if (bps == ss && pow2 && rsvd && nfats && fatsz16 == 0 && fatsz32 && root >= 2 && total) {
            auto* f = static_cast<Fat*>(kzalloc(sizeof(Fat)));
            f->vol = v;
            f->bps = bps;
            f->spc = spc;
            f->rsvd = rsvd;
            f->nfats = nfats;
            f->fatsz = fatsz32;
            f->root = root;
            f->data_start = rsvd + nfats * fatsz32;
            f->nclusters = (total - f->data_start) / spc;
            f->cbytes = bps * spc;
            f->fsinfo = uint32_t(bs[48] | (bs[49] << 8));
            f->ro = true;
            f->hint = 2;
            f->fatbuf = static_cast<uint8_t*>(kmalloc(bps));
            f->fatbuf_sec = 0xFFFFFFFF;
            f->secbuf = static_cast<uint8_t*>(kmalloc(bps));
            f->cbuf = static_cast<uint8_t*>(kmalloc(f->cbytes));
            memcpy(f->label, bs + 71, 11);
            for (int i = 10; i >= 0 && f->label[i] == ' '; i--) f->label[i] = 0;
            if (f->fatbuf && f->secbuf && f->cbuf) result = f;
        }
    }
    kfree(bs);
    return result;
}

int g_mounts = 0;

}  // namespace

int mount_all(void (*log)(const char*), bool all_rw) {
    char line[200];
    int mounted = 0;
    for (int d = 0; d < blk::device_count(); d++) {
        blk::Device* dev = blk::device(d);
        blk::Volume vols[16];
        int nv = blk::scan(dev, vols, 16);
        if (nv == 0) {
            vols[0] = {dev, 0, dev->sectors, 0, "whole disk"};
            nv = 1;
        }
        for (int i = 0; i < nv; i++) {
            Fat* f = try_open(vols[i]);
            if (!f) {
                ksnprintf(line, sizeof(line), "  %s part %d (%s): not FAT32, skipped", dev->name, vols[i].index,
                          vols[i].type);
                if (log) log(line);
                continue;
            }
            char mname[16];
            ksnprintf(mname, sizeof(mname), "usb%d", g_mounts);
            fs::Node* root = fs::mount(mname, &kOps, f, f->root, 0, 0);
            if (!root) continue;
            g_mounts++;
            char probe[64];
            ksnprintf(probe, sizeof(probe), "/%s/boot/limine/limine.conf", mname);
            bool boot = fs::lookup(probe) != nullptr;
            f->ro = !(boot || all_rw);
            if (!f->ro && f->fsinfo && f->fsinfo < f->rsvd) {
                // Our free-cluster bookkeeping is not tracked: mark FSInfo hints as unknown.
                if (blk::read(f->vol, f->fsinfo, 1, f->secbuf) && memcmp(f->secbuf, "RRaA", 4) == 0) {
                    memset(f->secbuf + 488, 0xFF, 8);
                    blk::write(f->vol, f->fsinfo, 1, f->secbuf);
                }
            }
            ksnprintf(line, sizeof(line), "  /%s: FAT32 \"%s\" on %s part %d, %lu MiB, cluster %u B, %s%s", mname,
                      f->label, dev->name, vols[i].index, uint64_t(f->nclusters) * f->cbytes >> 20, f->cbytes,
                      f->ro ? "read-only" : "read-write", boot ? " (boot drive)" : "");
            if (log) log(line);
            mounted++;
        }
    }
    return mounted;
}

}  // namespace fat
