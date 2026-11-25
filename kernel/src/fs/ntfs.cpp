// SPDX-License-Identifier: GPL-2.0-only
// NTFS, read-only. Supports: MFT with fixups, resident and non-resident
// attributes, fragmented files (data runs, sparse runs), $ATTRIBUTE_LIST,
// directory B+trees ($INDEX_ROOT / $INDEX_ALLOCATION / $BITMAP), long names.
// Not supported: compressed and encrypted files (reading them fails).
#include "fs/ntfs.h"
#include "fs/vfs.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"

namespace ntfs {
namespace {

constexpr uint32_t AT_ATTRIBUTE_LIST = 0x20, AT_VOLUME_NAME = 0x60, AT_DATA = 0x80,
                   AT_INDEX_ROOT = 0x90, AT_INDEX_ALLOCATION = 0xA0, AT_BITMAP = 0xB0, AT_END = 0xFFFFFFFF;
constexpr uint64_t REF_MASK = 0x0000FFFFFFFFFFFFull;
constexpr size_t MAX_FILE = 512ull << 20;  // files are cached whole in RAM
constexpr int MAX_DEPTH_RECORDS = 64;      // attribute list extension records per file

struct Run {
    uint64_t vcn, lcn, len;  // lcn = ~0 for sparse
};

struct Runs {
    Run* r = nullptr;
    int n = 0, cap = 0;
    bool add(uint64_t vcn, uint64_t lcn, uint64_t len) {
        if (n == cap) {
            int nc = cap ? cap * 2 : 16;
            auto* q = static_cast<Run*>(krealloc(r, sizeof(Run) * size_t(nc)));
            if (!q) return false;
            r = q;
            cap = nc;
        }
        r[n++] = {vcn, lcn, len};
        return true;
    }
    void clear() {
        kfree(r);
        r = nullptr;
        n = cap = 0;
    }
};

struct X {
    fs::MountBase base;
    blk::Volume vol;
    uint32_t ss;        // sector size
    uint32_t cbytes;    // cluster size
    uint32_t rec_size;  // MFT record size
    uint64_t nclusters;
    Runs mft;           // runs of $MFT's data
    uint8_t* io;        // 64 KiB scratch for sector I/O
    char label[64];
};

X* M(fs::Node* n) { return static_cast<X*>(n->mnt); }

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }
uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) | (uint64_t(rd32(p + 4)) << 32); }

size_t utf16_to_utf8(const uint8_t* in, int n, char* out, size_t cap) {
    size_t o = 0;
    for (int i = 0; i < n && o + 4 < cap; i++) {
        uint32_t c = rd16(in + 2 * i);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
            uint32_t lo = rd16(in + 2 * (i + 1));
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (c == '/') c = '_';
        if (c < 0x80) {
            out[o++] = char(c);
        } else if (c < 0x800) {
            out[o++] = char(0xC0 | (c >> 6));
            out[o++] = char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[o++] = char(0xE0 | (c >> 12));
            out[o++] = char(0x80 | ((c >> 6) & 0x3F));
            out[o++] = char(0x80 | (c & 0x3F));
        } else {
            out[o++] = char(0xF0 | (c >> 18));
            out[o++] = char(0x80 | ((c >> 12) & 0x3F));
            out[o++] = char(0x80 | ((c >> 6) & 0x3F));
            out[o++] = char(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
    return o;
}

// Byte-granular volume read through the scratch buffer.
bool read_bytes(X* x, uint64_t off, size_t len, uint8_t* out) {
    const uint32_t ss = x->ss;
    const uint32_t max_secs = 65536 / ss;
    while (len) {
        uint64_t lba = off / ss;
        uint32_t skip = uint32_t(off % ss);
        uint64_t need = (skip + len + ss - 1) / ss;
        uint32_t secs = need > max_secs ? max_secs : uint32_t(need);
        if (lba + secs > x->vol.sectors) return false;
        if (!blk::read(x->vol, lba, secs, x->io)) return false;
        size_t chunk = size_t(secs) * ss - skip;
        if (chunk > len) chunk = len;
        memcpy(out, x->io + skip, chunk);
        out += chunk;
        off += chunk;
        len -= chunk;
    }
    return true;
}

// Decodes a mapping-pairs array into `runs` (appending).
bool decode_runs(X* x, const uint8_t* p, const uint8_t* end, uint64_t vcn, Runs& runs) {
    uint64_t lcn = 0;
    while (p < end && *p) {
        int ls = *p & 0x0F, os = *p >> 4;
        p++;
        if (ls == 0 || ls > 8 || os > 8 || p + ls + os > end) return false;
        uint64_t len = 0;
        for (int i = 0; i < ls; i++) len |= uint64_t(p[i]) << (8 * i);
        p += ls;
        if (os == 0) {
            if (!runs.add(vcn, ~0ull, len)) return false;
        } else {
            int64_t d = 0;
            for (int i = 0; i < os; i++) d |= int64_t(p[i]) << (8 * i);
            if (p[os - 1] & 0x80) d |= -(int64_t(1) << (8 * os));  // sign-extend
            p += os;
            lcn = uint64_t(int64_t(lcn) + d);
            if (lcn + len > x->nclusters) return false;
            if (!runs.add(vcn, lcn, len)) return false;
        }
        vcn += len;
    }
    return true;
}

// Reads [off, off+len) of a non-resident stream; sparse parts read as zeros.
bool read_runs(X* x, const Runs& runs, uint64_t off, size_t len, uint8_t* out) {
    while (len) {
        uint64_t vcn = off / x->cbytes;
        const Run* r = nullptr;
        for (int i = 0; i < runs.n; i++)
            if (vcn >= runs.r[i].vcn && vcn < runs.r[i].vcn + runs.r[i].len) {
                r = &runs.r[i];
                break;
            }
        if (!r) return false;
        uint64_t run_end = (r->vcn + r->len) * x->cbytes;
        size_t chunk = run_end - off < len ? size_t(run_end - off) : len;
        if (r->lcn == ~0ull) {
            memset(out, 0, chunk);
        } else {
            uint64_t disk = r->lcn * x->cbytes + (off - r->vcn * x->cbytes);
            if (!read_bytes(x, disk, chunk, out)) return false;
        }
        out += chunk;
        off += chunk;
        len -= chunk;
    }
    return true;
}

// Applies the update sequence array of a "FILE" or "INDX" record in place.
bool fixup(uint8_t* rec, size_t size, const char* magic) {
    if (memcmp(rec, magic, 4) != 0) return false;
    uint16_t uo = rd16(rec + 4), uc = rd16(rec + 6);
    if (uc == 0 || uo + uc * 2u > size || (uc - 1u) * 512u > size) return false;
    const uint8_t* usa = rec + uo;
    for (uint16_t i = 1; i < uc; i++) {
        uint8_t* tail = rec + i * 512 - 2;
        if (tail[0] != usa[0] || tail[1] != usa[1]) return false;  // torn write
        tail[0] = usa[2 * i];
        tail[1] = usa[2 * i + 1];
    }
    return true;
}

bool read_record(X* x, uint64_t num, uint8_t* rec) {
    if (!read_runs(x, x->mft, num * x->rec_size, x->rec_size, rec)) return false;
    if (!fixup(rec, x->rec_size, "FILE")) return false;
    return rd16(rec + 22) & 1;  // in use
}

// Iterates the attributes of one record.
template <typename F>
void each_attr(X* x, uint8_t* rec, F&& f) {
    uint32_t off = rd16(rec + 20);
    uint32_t used = rd32(rec + 24);
    if (used > x->rec_size) used = x->rec_size;
    while (off + 16 <= used) {
        uint8_t* a = rec + off;
        uint32_t type = rd32(a), len = rd32(a + 4);
        if (type == AT_END || len < 16 || off + len > used) break;
        if (!f(a, type, len)) return;
        off += len;
    }
}

bool attr_unnamed(const uint8_t* a) { return a[9] == 0; }
bool attr_named_i30(const uint8_t* a) {
    if (a[9] != 4) return false;
    const uint8_t* n = a + rd16(a + 10);
    return rd16(n) == '$' && rd16(n + 2) == 'I' && rd16(n + 4) == '3' && rd16(n + 6) == '0';
}

// A located attribute stream: resident bytes or a run list.
struct Stream {
    bool found = false, resident = false, unsupported = false;
    uint8_t* data = nullptr;  // resident copy
    uint64_t size = 0, init = 0;
    Runs runs;
    void clear() {
        kfree(data);
        data = nullptr;
        runs.clear();
        found = resident = unsupported = false;
        size = init = 0;
    }
};

// Adds one attribute instance (base or extension record) to `s`.
bool take_attr(X* x, const uint8_t* a, uint32_t len, Stream& s) {
    if (a[8] == 0) {  // resident
        uint32_t vl = rd32(a + 16);
        uint16_t vo = rd16(a + 20);
        if (vo + vl > len) return false;
        s.data = static_cast<uint8_t*>(kmalloc(vl ? vl : 1));
        if (!s.data) return false;
        memcpy(s.data, a + vo, vl);
        s.found = s.resident = true;
        s.size = s.init = vl;
        return true;
    }
    uint16_t flags = rd16(a + 12);
    if (flags & 0x4001) s.unsupported = true;  // compressed or encrypted
    uint64_t start_vcn = rd64(a + 16);
    if (start_vcn == 0) {
        s.size = rd64(a + 48);
        s.init = rd64(a + 56);
    }
    uint16_t ro = rd16(a + 32);
    if (ro >= len) return false;
    s.found = true;
    return decode_runs(x, a + ro, a + len, start_vcn, s.runs);
}

// Finds attribute `type` (unnamed, or $I30 when i30) of MFT record `num`,
// following $ATTRIBUTE_LIST into extension records.
bool find_stream(X* x, uint64_t num, uint32_t type, bool i30, Stream& s) {
    auto* rec = static_cast<uint8_t*>(kmalloc(x->rec_size));
    if (!rec) return false;
    bool ok = read_record(x, num, rec);
    uint8_t* list = nullptr;
    uint64_t list_len = 0;
    if (ok) {
        each_attr(x, rec, [&](uint8_t* a, uint32_t t, uint32_t len) {
            if (t == AT_ATTRIBUTE_LIST) {
                Stream ls;
                if (take_attr(x, a, len, ls)) {
                    list_len = ls.size;
                    if (ls.resident) {
                        list = ls.data;
                        ls.data = nullptr;
                    } else if (list_len < (1u << 20)) {
                        list = static_cast<uint8_t*>(kmalloc(size_t(list_len)));
                        if (list && !read_runs(x, ls.runs, 0, size_t(list_len), list)) {
                            kfree(list);
                            list = nullptr;
                        }
                    }
                }
                ls.clear();
                return false;
            }
            return true;
        });
        if (!list) {
            each_attr(x, rec, [&](uint8_t* a, uint32_t t, uint32_t len) {
                if (t == type && (i30 ? attr_named_i30(a) : attr_unnamed(a))) {
                    ok = take_attr(x, a, len, s);
                    return false;
                }
                return true;
            });
        } else {
            // Attribute list: entries are sorted by type, then start VCN.
            uint64_t visited[MAX_DEPTH_RECORDS];
            int nvisited = 0;
            for (uint64_t off = 0; ok && off + 26 <= list_len;) {
                const uint8_t* e = list + off;
                uint32_t t = rd32(e);
                uint16_t el = rd16(e + 4);
                if (el < 26) break;
                bool name_ok = i30 ? (e[6] == 4) : (e[6] == 0);
                uint64_t ref = rd64(e + 16) & REF_MASK;
                bool seen = false;
                for (int k = 0; k < nvisited; k++) seen |= visited[k] == ref;
                if (t == type && name_ok && !seen && nvisited < MAX_DEPTH_RECORDS) {
                    visited[nvisited++] = ref;
                    auto* ext = ref == num ? rec : static_cast<uint8_t*>(kmalloc(x->rec_size));
                    if (ext != rec && (!ext || !read_record(x, ref, ext))) {
                        ok = false;
                    } else {
                        each_attr(x, ext, [&](uint8_t* a, uint32_t at, uint32_t len) {
                            if (at == type && (i30 ? attr_named_i30(a) : attr_unnamed(a)))
                                if (!take_attr(x, a, len, s)) ok = false;
                            return true;
                        });
                    }
                    if (ext != rec) kfree(ext);
                }
                off += el;
            }
            kfree(list);
        }
    }
    kfree(rec);
    return ok && s.found;
}

bool read_stream(X* x, Stream& s, uint64_t off, size_t len, uint8_t* out) {
    if (s.resident) {
        if (off + len > s.size) return false;
        memcpy(out, s.data + off, len);
        return true;
    }
    if (!read_runs(x, s.runs, off, len, out)) return false;
    // Bytes past the initialized size are zero by definition.
    if (off + len > s.init) {
        uint64_t z = s.init > off ? s.init - off : 0;
        memset(out + z, 0, size_t(len - z));
    }
    return true;
}

// Walks the index entries in [p, end) and attaches files to `dir`.
bool add_entries(X* x, fs::Node* dir, const uint8_t* p, const uint8_t* end) {
    (void)x;
    while (p + 16 <= end) {
        uint16_t el = rd16(p + 8), kl = rd16(p + 10), fl = rd16(p + 12);
        if (fl & 2) break;  // last entry
        if (el < 16 || p + el > end) return false;
        if (kl >= 66) {
            const uint8_t* k = p + 16;
            uint64_t ref = rd64(p) & REF_MASK;
            uint8_t nl = k[64], ns = k[65];
            uint32_t fattr = rd32(k + 56);
            bool is_dir = fattr & 0x10000000;
            bool hidden_sys = ref < 24 || (nl > 0 && rd16(k + 66) == '$' && ref < 64);
            if (ns != 2 && !hidden_sys && 66u + nl * 2u <= kl) {
                char name[fs::NAME_MAX * 3 + 4];
                utf16_to_utf8(k + 66, nl, name, sizeof(name));
                if (strlen(name) > fs::NAME_MAX) name[fs::NAME_MAX] = 0;
                if (name[0] && !fs::find_child(dir, name)) {
                    fs::Node* n = fs::attach(dir, name, is_dir);
                    if (!n) return false;
                    n->fs_a = ref;
                    if (!is_dir) n->size = size_t(rd64(k + 48));
                }
            }
        }
        p += el;
    }
    return true;
}

bool load_dir(fs::Node* dir) {
    X* x = M(dir);
    Stream root;
    if (!find_stream(x, dir->fs_a, AT_INDEX_ROOT, true, root) || !root.resident || root.size < 32) {
        root.clear();
        return false;
    }
    uint32_t block = rd32(root.data + 8);
    const uint8_t* ih = root.data + 16;
    bool ok = add_entries(x, dir, ih + rd32(ih), ih + rd32(ih + 4));
    bool large = ih[12] & 1;
    root.clear();
    if (!ok || !large) return ok;

    Stream alloc, bmp;
    if (!find_stream(x, dir->fs_a, AT_INDEX_ALLOCATION, true, alloc) || block < 512 || block > 65536) {
        alloc.clear();
        return false;
    }
    bool have_bmp = find_stream(x, dir->fs_a, AT_BITMAP, true, bmp);
    uint8_t* bm = nullptr;
    uint64_t bm_len = 0;
    if (have_bmp && bmp.size < (1u << 20)) {
        bm_len = bmp.size;
        bm = static_cast<uint8_t*>(kmalloc(size_t(bm_len ? bm_len : 1)));
        if (bm && !read_stream(x, bmp, 0, size_t(bm_len), bm)) {
            kfree(bm);
            bm = nullptr;
        }
    }
    bmp.clear();
    auto* buf = static_cast<uint8_t*>(kmalloc(block));
    uint64_t nblocks = alloc.size / block;
    for (uint64_t i = 0; ok && buf && i < nblocks; i++) {
        if (bm && (i / 8 >= bm_len || !(bm[i / 8] & (1u << (i % 8))))) continue;  // unused block
        if (!read_stream(x, alloc, i * block, block, buf)) {
            ok = false;
            break;
        }
        if (!fixup(buf, block, "INDX")) continue;
        const uint8_t* h = buf + 24;
        uint32_t eo = rd32(h), es = rd32(h + 4);
        if (24 + es > block) continue;
        ok = add_entries(x, dir, h + eo, h + es);
    }
    kfree(buf);
    kfree(bm);
    alloc.clear();
    return ok && buf;
}

bool load_file(fs::Node* n) {
    X* x = M(n);
    Stream s;
    if (!find_stream(x, n->fs_a, AT_DATA, false, s) || s.unsupported || s.size > MAX_FILE) {
        s.clear();
        return false;
    }
    size_t size = size_t(s.size);
    auto* buf = static_cast<uint8_t*>(kmalloc(size ? size : 1));
    bool ok = buf && read_stream(x, s, 0, size, buf);
    s.clear();
    n->size = 0;
    ok = ok && fs::set_data(n, buf, size);
    kfree(buf);
    return ok;
}

fs::Error ro_create(fs::Node*, fs::Node*) { return fs::ReadOnly; }
fs::Error ro_write(fs::Node*, const void*, size_t) { return fs::ReadOnly; }
fs::Error ro_remove(fs::Node*) { return fs::ReadOnly; }

const fs::Ops kOps = {load_dir, load_file, ro_create, ro_write, ro_remove};

}  // namespace

bool probe_mount(const blk::Volume& v, const char* mname, bool, char* info, size_t cap) {
    uint32_t ss = v.dev->sector_size;
    auto* bs = static_cast<uint8_t*>(kmalloc(ss));
    if (!bs) return false;
    bool ok = blk::read(v, 0, 1, bs) && memcmp(bs + 3, "NTFS    ", 8) == 0 && rd16(bs + 510) == 0xAA55;
    if (!ok) {
        kfree(bs);
        return false;
    }
    uint32_t bps = rd16(bs + 11);
    uint32_t spc = bs[13] <= 0x80 ? bs[13] : 1u << (256 - bs[13]);
    int8_t cpr = int8_t(bs[64]);
    uint64_t total = rd64(bs + 40);
    uint64_t mft_lcn = rd64(bs + 48);
    kfree(bs);
    if (bps != ss || spc == 0 || bps * spc > (2u << 20)) return false;

    auto* x = static_cast<X*>(kzalloc(sizeof(X)));
    if (!x) return false;
    x->vol = v;
    x->ss = ss;
    x->cbytes = bps * spc;
    x->rec_size = cpr > 0 ? uint32_t(cpr) * x->cbytes : 1u << (-cpr);
    x->nclusters = total / spc;
    x->io = static_cast<uint8_t*>(kmalloc(65536));
    auto* rec = static_cast<uint8_t*>(kmalloc(x->rec_size));
    if (!x->io || !rec || x->rec_size < 512 || x->rec_size > 65536) return false;

    // Bootstrap: record 0 ($MFT) lives at mft_lcn; its $DATA gives the full MFT layout.
    if (!read_bytes(x, mft_lcn * x->cbytes, x->rec_size, rec) || !fixup(rec, x->rec_size, "FILE")) {
        kfree(rec);
        return false;
    }
    each_attr(x, rec, [&](uint8_t* a, uint32_t t, uint32_t len) {
        if (t == AT_DATA && attr_unnamed(a) && a[8] == 1) {
            uint16_t ro = rd16(a + 32);
            ok = ro < len && decode_runs(x, a + ro, a + len, rd64(a + 16), x->mft);
            return false;
        }
        return true;
    });
    if (!ok || x->mft.n == 0) {
        kfree(rec);
        return false;
    }
    // Volume label from $Volume (record 3).
    if (read_record(x, 3, rec)) {
        each_attr(x, rec, [&](uint8_t* a, uint32_t t, uint32_t) {
            if (t == AT_VOLUME_NAME && a[8] == 0) {
                utf16_to_utf8(a + rd16(a + 20), int(rd32(a + 16) / 2), x->label, sizeof(x->label));
                return false;
            }
            return true;
        });
    }
    kfree(rec);
    x->base.ro = true;
    x->base.can_write = false;
    if (!fs::mount(mname, &kOps, x, 5, 0, 0)) return false;  // record 5 = root directory
    ksnprintf(info, cap, "NTFS \"%s\", %lu MiB, cluster %u B, read-only", x->label,
              (x->nclusters * x->cbytes) >> 20, x->cbytes);
    return true;
}

}  // namespace ntfs
