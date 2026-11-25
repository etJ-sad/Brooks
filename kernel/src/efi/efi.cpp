// SPDX-License-Identifier: GPL-2.0-only
#include "efi/efi.h"
#include <stdarg.h>
#include "arch/x86_64/cpu.h"
#include "fs/vfs.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

namespace efi {
namespace {

#define EFIAPI __attribute__((ms_abi))
using Status = uint64_t;
constexpr Status SUCCESS = 0;
constexpr Status BUFFER_TOO_SMALL = 0x8000000000000005ull;

struct TableHeader {
    uint64_t signature;
    uint32_t revision, header_size, crc32, reserved;
};

struct SystemTable {
    TableHeader hdr;
    uint64_t firmware_vendor;  // CHAR16*
    uint32_t firmware_revision;
    uint32_t pad;
    uint64_t con_in_handle, con_in, con_out_handle, con_out, stderr_handle, stderr_;
    uint64_t runtime_services, boot_services;
    uint64_t n_config, config;
};

struct RuntimeServices {
    TableHeader hdr;
    Status(EFIAPI* get_time)(Time*, void*);
    void* set_time;
    void* get_wakeup_time;
    void* set_wakeup_time;
    void* set_virtual_address_map;
    void* convert_pointer;
    Status(EFIAPI* get_variable)(const char16_t*, const Guid*, uint32_t*, uint64_t*, void*);
    Status(EFIAPI* get_next_variable_name)(uint64_t*, char16_t*, Guid*);
    Status(EFIAPI* set_variable)(const char16_t*, const Guid*, uint32_t, uint64_t, const void*);
    void* get_next_high_monotonic_count;
    void(EFIAPI* reset_system)(uint32_t, Status, uint64_t, const void*);
    void* update_capsule;
    void* query_capsule_capabilities;
    Status(EFIAPI* query_variable_info)(uint32_t, uint64_t*, uint64_t*, uint64_t*);
};

struct MemoryDescriptor {
    uint32_t type, pad;
    uint64_t phys, virt, pages, attr;
};

constexpr uint64_t ST_SIGNATURE = 0x5453595320494249ull;  // "IBI SYST"
constexpr uint64_t RT_SIGNATURE = 0x56524553544e5552ull;  // "RUNTSERV"
constexpr uint64_t ATTR_RUNTIME = 1ull << 63, ATTR_WB = 1ull << 3;
constexpr uint32_t VAR_NV = 1, VAR_BS = 2, VAR_RT = 4;

constexpr Guid GLOBAL = {0x8BE4DF61, 0x93CA, 0x11D2, {0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C}};
constexpr Guid SECURITY_DB = {0xD719B2CB, 0x3D3A, 0x4596, {0xA3, 0xBC, 0xDA, 0xD0, 0x0E, 0x67, 0x65, 0x6F}};

SystemTable* g_st = nullptr;
RuntimeServices* g_rt = nullptr;
bool g_ok = false;
char g_status[160] = "off";
char g_vendor[64] = "?";
int g_regions = 0;
uint64_t g_mapped = 0;

bool guid_eq(const Guid& x, const Guid& y) { return memcmp(&x, &y, sizeof(Guid)) == 0; }

// The firmware may use SSE/x87: make sure both are usable (our own kernel
// code never touches them, so there is no state to preserve).
void enable_fpu_sse() {
    uint64_t cr0, cr4;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 & ~((1ull << 2) | (1ull << 3))) | (1ull << 1);  // EM=0, TS=0, MP=1
    asm volatile("mov %0, %%cr0" ::"r"(cr0));
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ull << 9) | (1ull << 10);  // OSFXSR, OSXMMEXCPT
    asm volatile("mov %0, %%cr4" ::"r"(cr4));
    asm volatile("fninit");
}

bool range_mapped(uint64_t phys, uint64_t len, const void* mm, uint64_t mm_size, uint64_t dsz) {
    auto* p = static_cast<const uint8_t*>(mm);
    for (uint64_t off = 0; off + sizeof(MemoryDescriptor) <= mm_size; off += dsz) {
        auto* d = reinterpret_cast<const MemoryDescriptor*>(p + off);
        if (!(d->attr & ATTR_RUNTIME)) continue;
        if (phys >= d->phys && phys + len <= d->phys + d->pages * mm::PAGE) return true;
    }
    return false;
}

void utf16_to_utf8(const char16_t* s, size_t max_units, char* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < max_units && s[i] && o + 4 < cap; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < max_units && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
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
}

// Growable text for the /sys/efi files.
struct Text {
    char* p = nullptr;
    size_t n = 0, cap = 0;
    __attribute__((format(printf, 2, 3))) void add(const char* fmt, ...) {
        char line[512];
        va_list ap;
        va_start(ap, fmt);
        kvsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        size_t l = strlen(line);
        if (n + l + 1 > cap) {
            size_t nc = cap ? cap * 2 : 4096;
            while (nc < n + l + 1) nc *= 2;
            char* q = static_cast<char*>(krealloc(p, nc));
            if (!q) return;
            p = q;
            cap = nc;
        }
        memcpy(p + n, line, l);
        n += l;
        p[n] = 0;
    }
    ~Text() { kfree(p); }
};

void guid_str(const Guid& g, char* out, size_t cap) {
    ksnprintf(out, cap, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.a, g.b, g.c, g.d[0], g.d[1], g.d[2],
              g.d[3], g.d[4], g.d[5], g.d[6], g.d[7]);
}

const char* guid_name(const Guid& g) {
    if (guid_eq(g, GLOBAL)) return "EFI global";
    if (guid_eq(g, SECURITY_DB)) return "Secure Boot db";
    return nullptr;
}

// Short text form of a UEFI device path (the parts that matter for boot entries).
void device_path_str(const uint8_t* p, size_t len, char* out, size_t cap) {
    size_t o = 0;
    out[0] = 0;
    auto put = [&](const char* s) {
        while (*s && o + 1 < cap) out[o++] = *s++;
        out[o] = 0;
    };
    size_t off = 0;
    while (off + 4 <= len) {
        uint8_t type = p[off], sub = p[off + 1];
        uint16_t nl = uint16_t(p[off + 2] | (p[off + 3] << 8));
        if (nl < 4 || off + nl > len || type == 0x7F) break;
        char tmp[192];
        tmp[0] = 0;
        if (type == 3 && sub == 23) ksnprintf(tmp, sizeof(tmp), "NVMe/");
        else if (type == 3 && sub == 18) ksnprintf(tmp, sizeof(tmp), "SATA/");
        else if (type == 3 && sub == 5) ksnprintf(tmp, sizeof(tmp), "USB/");
        else if (type == 3 && sub == 11) ksnprintf(tmp, sizeof(tmp), "MAC/");
        else if (type == 3 && (sub == 12 || sub == 13)) ksnprintf(tmp, sizeof(tmp), "IP/");
        else if (type == 3 && sub == 24) ksnprintf(tmp, sizeof(tmp), "URI/");
        else if (type == 4 && sub == 1 && nl >= 8) {
            uint32_t part = uint32_t(p[off + 4] | (p[off + 5] << 8) | (p[off + 6] << 16) | (uint32_t(p[off + 7]) << 24));
            ksnprintf(tmp, sizeof(tmp), "HD%u", part);
        } else if (type == 4 && sub == 4) {
            char16_t name[96];
            size_t units = (nl - 4) / 2 < 95 ? (nl - 4) / 2 : 95;
            memcpy(name, p + off + 4, units * 2);
            name[units] = 0;
            tmp[0] = ' ';
            utf16_to_utf8(name, units, tmp + 1, sizeof(tmp) - 1);
        } else if (type == 5) {
            ksnprintf(tmp, sizeof(tmp), "BIOS-entry");
        } else if (type == 4 && sub == 6) {
            ksnprintf(tmp, sizeof(tmp), "FvFile");
        }
        put(tmp);
        off += nl;
    }
}

bool get_u64(const char16_t* name, uint64_t& v) {
    v = 0;
    size_t sz = sizeof(v);
    return get_variable(name, GLOBAL, &v, sz) && sz >= 1;
}

void make_name(char16_t* out, const char* prefix, unsigned num) {
    char tmp[16];
    ksnprintf(tmp, sizeof(tmp), "%s%04X", prefix, num);
    size_t i = 0;
    for (; tmp[i]; i++) out[i] = char16_t(tmp[i]);
    out[i] = 0;
}

}  // namespace

bool init(uint64_t st_phys, const void* memmap, uint64_t memmap_size, uint64_t desc_size, bool enabled) {
    if (!enabled) {
        ksnprintf(g_status, sizeof(g_status), "runtime services off (efi=off)");
        return false;
    }
    if (!st_phys) {
        ksnprintf(g_status, sizeof(g_status), "no EFI system table from the bootloader");
        return false;
    }
    if (!memmap || !desc_size || desc_size < sizeof(MemoryDescriptor)) {
        ksnprintf(g_status, sizeof(g_status), "no EFI memory map from the bootloader");
        return false;
    }
    // Identity-map every runtime region: code, data and the firmware's MMIO
    // (SPI flash, RTC, reset registers).
    auto* p = static_cast<const uint8_t*>(memmap);
    for (uint64_t off = 0; off + sizeof(MemoryDescriptor) <= memmap_size; off += desc_size) {
        auto* d = reinterpret_cast<const MemoryDescriptor*>(p + off);
        if (!(d->attr & ATTR_RUNTIME) || d->pages == 0) continue;
        const bool mmio = d->type == 11 || d->type == 12;
        const bool wb = !mmio && (d->attr & ATTR_WB);
        if (!vmm::map_identity(d->phys, d->pages * mm::PAGE, wb ? vmm::WriteBack : vmm::Uncached, !mmio)) {
            ksnprintf(g_status, sizeof(g_status), "cannot map runtime region 0x%lx (%lu pages)", d->phys, d->pages);
            return false;
        }
        g_regions++;
        g_mapped += d->pages * mm::PAGE;
    }
    if (!g_regions) {
        ksnprintf(g_status, sizeof(g_status), "EFI memory map has no runtime regions");
        return false;
    }
    if (!range_mapped(st_phys, sizeof(SystemTable), memmap, memmap_size, desc_size) &&
        !vmm::map_identity(st_phys, sizeof(SystemTable), vmm::WriteBack, false)) {
        ksnprintf(g_status, sizeof(g_status), "cannot map system table");
        return false;
    }
    g_st = reinterpret_cast<SystemTable*>(st_phys);
    if (g_st->hdr.signature != ST_SIGNATURE) {
        ksnprintf(g_status, sizeof(g_status), "bad system table signature at 0x%lx", st_phys);
        return false;
    }
    uint64_t rt = g_st->runtime_services;
    if (!rt || !range_mapped(rt, sizeof(TableHeader), memmap, memmap_size, desc_size)) {
        ksnprintf(g_status, sizeof(g_status), "runtime services table 0x%lx not in a runtime region", rt);
        return false;
    }
    g_rt = reinterpret_cast<RuntimeServices*>(rt);
    if (g_rt->hdr.signature != RT_SIGNATURE) {
        ksnprintf(g_status, sizeof(g_status), "bad runtime services signature");
        return false;
    }
    if (g_st->firmware_vendor && range_mapped(g_st->firmware_vendor, 2, memmap, memmap_size, desc_size))
        utf16_to_utf8(reinterpret_cast<const char16_t*>(g_st->firmware_vendor), 60, g_vendor, sizeof(g_vendor));
    enable_fpu_sse();
    g_ok = true;
    const uint32_t rev = g_st->hdr.revision;
    ksnprintf(g_status, sizeof(g_status), "UEFI %u.%u, '%s' rev 0x%x, %d runtime regions (%lu KiB) mapped", rev >> 16,
              rev & 0xFFFF, g_vendor, g_st->firmware_revision, g_regions, g_mapped / 1024);
    return true;
}

bool available() { return g_ok; }
const char* status() { return g_status; }

bool get_time(Time& t) {
    if (!g_ok || !g_rt->get_time) return false;
    uint64_t f = cpu::save_irq();
    Status s = g_rt->get_time(&t, nullptr);
    cpu::restore_irq(f);
    return s == SUCCESS;
}

void reset(ResetType type) {
    if (!g_ok || !g_rt->reset_system) return;
    cpu::cli();
    g_rt->reset_system(type, SUCCESS, 0, nullptr);
    cpu::sti();
}

bool get_variable(const char16_t* name, const Guid& guid, void* buf, size_t& size, uint32_t* attrs) {
    if (!g_ok || !g_rt->get_variable) return false;
    uint64_t sz = size;
    uint32_t a = 0;
    uint64_t f = cpu::save_irq();
    Status s = g_rt->get_variable(name, &guid, &a, &sz, buf);
    cpu::restore_irq(f);
    if (s != SUCCESS) {
        if (s == BUFFER_TOO_SMALL) size = sz;
        return false;
    }
    size = sz;
    if (attrs) *attrs = a;
    return true;
}

bool firmware_setup_supported() {
    uint64_t sup = 0;
    return g_ok && get_u64(u"OsIndicationsSupported", sup) && (sup & 1);
}

bool request_firmware_setup() {
    if (!firmware_setup_supported() || !g_rt->set_variable) return false;
    uint64_t ind = 0;
    get_u64(u"OsIndications", ind);
    ind |= 1;  // EFI_OS_INDICATIONS_BOOT_TO_FW_UI
    uint64_t f = cpu::save_irq();
    Status s = g_rt->set_variable(u"OsIndications", &GLOBAL, VAR_NV | VAR_BS | VAR_RT, sizeof(ind), &ind);
    cpu::restore_irq(f);
    return s == SUCCESS;
}

void publish_files() {
    {
        Text t;
        t.add("UEFI firmware\n=============\n\n");
        if (!g_ok) {
            t.add("Runtime services: %s\n", g_status);
        } else {
            const uint32_t rev = g_st->hdr.revision;
            t.add("UEFI revision:     %u.%u\n", rev >> 16, rev & 0xFFFF);
            t.add("Firmware vendor:   %s\n", g_vendor);
            t.add("Firmware revision: 0x%08x\n", g_st->firmware_revision);
            t.add("Runtime regions:   %d, %lu KiB identity-mapped\n", g_regions, g_mapped / 1024);
            Time tm;
            if (get_time(tm)) {
                t.add("Firmware time:     %04u-%02u-%02u %02u:%02u:%02u", tm.year, tm.month, tm.day, tm.hour, tm.minute,
                      tm.second);
                if (tm.timezone == 2047) t.add(" (local time)\n");
                else t.add(" (UTC%+d min)\n", -int(tm.timezone));
            } else {
                t.add("Firmware time:     GetTime failed\n");
            }
            uint64_t sup = 0;
            if (get_u64(u"OsIndicationsSupported", sup))
                t.add("OsIndications:     0x%lx%s\n", sup, (sup & 1) ? " (boot to firmware setup supported)" : "");
            uint8_t sb = 0, sm = 0;
            size_t s1 = 1, s2 = 1;
            bool have_sb = get_variable(u"SecureBoot", GLOBAL, &sb, s1);
            bool have_sm = get_variable(u"SetupMode", GLOBAL, &sm, s2);
            t.add("Secure Boot:       %s%s\n", have_sb ? (sb ? "ON" : "off") : "?",
                  have_sm && sm ? " (setup mode)" : "");
            if (g_rt->hdr.header_size >= sizeof(RuntimeServices) && g_rt->query_variable_info) {
                uint64_t max = 0, rem = 0, maxv = 0;
                uint64_t f = cpu::save_irq();
                Status s = g_rt->query_variable_info(VAR_NV | VAR_BS | VAR_RT, &max, &rem, &maxv);
                cpu::restore_irq(f);
                if (s == SUCCESS)
                    t.add("NVRAM:             %lu KiB total, %lu KiB free, max variable %lu bytes\n", max / 1024,
                          rem / 1024, maxv);
            }
        }
        fs::put_file("/sys/efi/info.txt", t.p ? t.p : "", t.n);
    }
    if (!g_ok) return;

    {  // boot entries
        Text t;
        t.add("UEFI boot entries (BootOrder)\n=============================\n\n");
        uint16_t cur = 0xFFFF, next = 0xFFFF;
        size_t sz = 2;
        bool have_cur = get_variable(u"BootCurrent", GLOBAL, &cur, sz);
        sz = 2;
        bool have_next = get_variable(u"BootNext", GLOBAL, &next, sz);
        uint16_t order[64];
        size_t osz = sizeof(order);
        if (!get_variable(u"BootOrder", GLOBAL, order, osz)) osz = 0;
        if (have_cur) t.add("BootCurrent: Boot%04X\n", cur);
        if (have_next) t.add("BootNext:    Boot%04X\n", next);
        t.add("\n");
        auto* buf = static_cast<uint8_t*>(kmalloc(4096));
        for (size_t i = 0; buf && i < osz / 2; i++) {
            char16_t name[16];
            make_name(name, "Boot", order[i]);
            size_t bsz = 4096;
            if (!get_variable(name, GLOBAL, buf, bsz) || bsz < 6) {
                t.add("  Boot%04X  (unreadable)\n", order[i]);
                continue;
            }
            uint32_t attr = uint32_t(buf[0] | (buf[1] << 8) | (buf[2] << 16) | (uint32_t(buf[3]) << 24));
            uint16_t fpl = uint16_t(buf[4] | (buf[5] << 8));
            size_t units = 0;
            auto* desc = reinterpret_cast<const char16_t*>(buf + 6);
            while (6 + units * 2 + 1 < bsz && desc[units]) units++;
            char d8[160], dp[256];
            utf16_to_utf8(desc, units, d8, sizeof(d8));
            size_t dp_off = 6 + (units + 1) * 2;
            dp[0] = 0;
            if (dp_off + fpl <= bsz) device_path_str(buf + dp_off, fpl, dp, sizeof(dp));
            t.add("%s Boot%04X %s %-36s %s\n", have_cur && order[i] == cur ? "*" : " ", order[i],
                  (attr & 1) ? "   " : "off", d8, dp);
        }
        kfree(buf);
        t.add("\n* = entry this boot came from; off = entry disabled.\n");
        fs::put_file("/sys/efi/boot.txt", t.p, t.n);
    }

    {  // all variable names
        Text t;
        t.add("UEFI variables (name, vendor GUID, size)\n========================================\n\n");
        char16_t name[512];
        name[0] = 0;
        Guid g{};
        int count = 0;
        uint8_t dummy = 0;
        for (int guard = 0; guard < 4000 && g_rt->get_next_variable_name; guard++) {
            uint64_t nsz = sizeof(name);
            uint64_t f = cpu::save_irq();
            Status s = g_rt->get_next_variable_name(&nsz, name, &g);
            cpu::restore_irq(f);
            if (s != SUCCESS) break;  // NOT_FOUND = end of list (names over 255 chars are skipped)
            char n8[300], gs[40];
            utf16_to_utf8(name, 255, n8, sizeof(n8));
            guid_str(g, gs, sizeof(gs));
            size_t vsz = 0;
            uint32_t attrs = 0;
            uint64_t vs = 0;
            f = cpu::save_irq();
            Status s2 = g_rt->get_variable(name, &g, &attrs, &vs, &dummy);
            cpu::restore_irq(f);
            vsz = (s2 == SUCCESS || s2 == BUFFER_TOO_SMALL) ? size_t(vs) : 0;
            const char* gn = guid_name(g);
            t.add("%-32s %s %6lu B%s%s\n", n8, gs, uint64_t(vsz), gn ? "  " : "", gn ? gn : "");
            count++;
        }
        t.add("\n%d variables.\n", count);
        fs::put_file("/sys/efi/variables.txt", t.p, t.n);
    }
}

}  // namespace efi
