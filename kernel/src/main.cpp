// SPDX-License-Identifier: GPL-2.0-only
// Kernel entry point: boot log on GOP + serial (memory map, PCI scan, Intel
// NPU detection), then memory, ACPI, interrupts, keyboard, RAM file system
// and the built-in file manager.
#include <stddef.h>
#include <stdint.h>

#define LIMINE_API_REVISION 3
#include <limine.h>

#include "acpi/acpi.h"
#include "efi/efi.h"
#include "npu/npu.h"
#include "apps/apps.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/idt.h"
#include "drivers/keyboard.h"
#include "drivers/serial.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "gfx/console.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "pci/pci.h"
#include "version.h"

// ---------------------------------------------------------------- Limine requests
namespace {

[[gnu::used, gnu::section(".limine_requests")]] volatile LIMINE_BASE_REVISION(3);

[[gnu::used, gnu::section(".limine_requests")]] volatile limine_framebuffer_request fb_req = {
    .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0, .response = nullptr};
[[gnu::used, gnu::section(".limine_requests")]] volatile limine_memmap_request memmap_req = {
    .id = LIMINE_MEMMAP_REQUEST, .revision = 0, .response = nullptr};
[[gnu::used, gnu::section(".limine_requests")]] volatile limine_hhdm_request hhdm_req = {
    .id = LIMINE_HHDM_REQUEST, .revision = 0, .response = nullptr};
[[gnu::used, gnu::section(".limine_requests")]] volatile limine_executable_cmdline_request cmdline_req = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST, .revision = 0, .response = nullptr};
[[gnu::used, gnu::section(".limine_requests")]] volatile limine_bootloader_info_request bl_req = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST, .revision = 0, .response = nullptr};
[[gnu::used, gnu::section(".limine_requests")]] volatile limine_firmware_type_request fw_req = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST, .revision = 0, .response = nullptr};
[[gnu::used, gnu::section(".limine_requests")]] volatile limine_rsdp_request rsdp_req = {
    .id = LIMINE_RSDP_REQUEST, .revision = 0, .response = nullptr};
[[gnu::used, gnu::section(".limine_requests")]] volatile limine_efi_system_table_request efi_st_req = {
    .id = LIMINE_EFI_SYSTEM_TABLE_REQUEST, .revision = 0, .response = nullptr};
[[gnu::used, gnu::section(".limine_requests")]] volatile limine_efi_memmap_request efi_mm_req = {
    .id = LIMINE_EFI_MEMMAP_REQUEST, .revision = 0, .response = nullptr};
[[gnu::used, gnu::section(".limine_requests")]] volatile limine_module_request module_req = {
    .id = LIMINE_MODULE_REQUEST, .revision = 0, .response = nullptr,
    .internal_module_count = 0, .internal_modules = nullptr};

[[gnu::used, gnu::section(".limine_requests_start")]] volatile LIMINE_REQUESTS_START_MARKER;
[[gnu::used, gnu::section(".limine_requests_end")]] volatile LIMINE_REQUESTS_END_MARKER;

// ---------------------------------------------------------------- helpers

const char* size_str(uint64_t bytes, char* buf) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int u = 0;
    uint64_t whole = bytes, frac = 0;
    while (whole >= 1024 && u < 4) {
        frac = (whole % 1024) * 10 / 1024;
        whole /= 1024;
        u++;
    }
    // "12.3 GiB" / "640 KiB"
    char tmp[24];
    int n = 0;
    uint64_t w = whole;
    char rev[24];
    int rn = 0;
    do { rev[rn++] = char('0' + w % 10); w /= 10; } while (w);
    while (rn) tmp[n++] = rev[--rn];
    if (u > 0 && whole < 100 && frac) { tmp[n++] = '.'; tmp[n++] = char('0' + frac); }
    tmp[n++] = ' ';
    for (const char* s = units[u]; *s; s++) tmp[n++] = *s;
    tmp[n] = 0;
    memcpy(buf, tmp, size_t(n) + 1);
    return buf;
}

const char* memmap_type(uint64_t t, con::Color& c) {
    switch (t) {
    case LIMINE_MEMMAP_USABLE: c = con::Green; return "Usable";
    case LIMINE_MEMMAP_RESERVED: c = con::Dim; return "Reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE: c = con::Cyan; return "ACPI reclaimable";
    case LIMINE_MEMMAP_ACPI_NVS: c = con::Cyan; return "ACPI NVS";
    case LIMINE_MEMMAP_BAD_MEMORY: c = con::Red; return "BAD MEMORY";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: c = con::Default; return "Bootloader reclaim";
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: c = con::Magenta; return "Kernel+modules";
    case LIMINE_MEMMAP_FRAMEBUFFER: c = con::Yellow; return "Framebuffer";
    default: c = con::Red; return "Unknown";
    }
}

void cpu_brand(char out[49]) {
    uint32_t r[12];
    uint32_t a, b, c, d;
    cpu::cpuid(0x80000000, 0, a, b, c, d);
    if (a < 0x80000004) {
        memcpy(out, "unknown", 8);
        return;
    }
    for (uint32_t i = 0; i < 3; i++) cpu::cpuid(0x80000002 + i, 0, r[i * 4], r[i * 4 + 1], r[i * 4 + 2], r[i * 4 + 3]);
    memcpy(out, r, 48);
    out[48] = 0;
    // trim leading spaces
    int s = 0;
    while (out[s] == ' ') s++;
    if (s) memmove(out, out + s, size_t(49 - s));
}

void section(const char* title) {
    kcprintf(con::Cyan, "== %s ==", title);
    kprintf("\n");
}

// ---------------------------------------------------------------- boot log parts

struct Totals {
    uint64_t usable = 0, reserved = 0, total = 0, highest = 0;
    int raw_entries = 0, shown = 0;
};

Totals print_memmap(bool title) {
    Totals t;
    if (title) section("Memory map (adjacent entries of the same type merged)");
    auto* r = memmap_req.response;
    if (!r) {
        kcprintf(con::Red, "no memory map from bootloader!\n");
        return t;
    }
    t.raw_entries = int(r->entry_count);
    char sz[32];
    uint64_t i = 0;
    while (i < r->entry_count) {
        uint64_t base = r->entries[i]->base, len = r->entries[i]->length, type = r->entries[i]->type;
        uint64_t j = i + 1;
        while (j < r->entry_count && r->entries[j]->type == type && r->entries[j]->base == base + len) {
            len += r->entries[j]->length;
            j++;
        }
        con::Color c;
        const char* name = memmap_type(type, c);
        kprintf("%016lx-%016lx %9s ", base, base + len - 1, size_str(len, sz));
        kcprintf(c, "%s\n", name);
        t.shown++;
        if (type == LIMINE_MEMMAP_USABLE || type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
            type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES)
            t.usable += len;
        else
            t.reserved += len;
        if (type != LIMINE_MEMMAP_RESERVED && type != LIMINE_MEMMAP_FRAMEBUFFER) t.total += len;
        if (base + len > t.highest) t.highest = base + len;
        i = j;
    }
    kprintf("%d entries (%d raw).  ", t.shown, t.raw_entries);
    kcprintf(con::Green, "RAM usable: %s", size_str(t.usable, sz));
    kprintf("  top: 0x%lx\n", t.highest);
    return t;
}

int g_npu_index = -1;

void print_pci(bool title) {
    if (title) section("PCI devices (bus:dev.fn vendor:device class)");
    int n = pci::scan();
    for (int i = 0; i < n; i++) {
        const pci::Device& d = pci::at(i);
        const char* npu = pci::intel_npu_name(d);
        if (npu && g_npu_index < 0) g_npu_index = i;
        con::Color c = npu ? con::Green : (d.cls == 0x06 ? con::Dim : con::Default);
        kcprintf(c, "%02x:%02x.%x %04x:%04x %02x%02x%02x %-22s %s", d.bus, d.dev, d.fn, d.vendor, d.device, d.cls,
                 d.subcls, d.progif, pci::class_name(d.cls, d.subcls, d.progif), pci::vendor_name(d.vendor));
        if (npu) kcprintf(con::Green, "  <== NPU");
        kprintf("\n");
    }
    kprintf("%d PCI functions.\n", n);
}

void print_bars(const pci::Device& d) {
    for (int b = 0; b < 6; b++) {
        uint32_t lo = pci::read32(d.bus, d.dev, d.fn, uint8_t(0x10 + b * 4));
        if (lo == 0) continue;
        if (lo & 1) {
            kprintf("  BAR%d I/O  0x%x\n", b, lo & ~3u);
            continue;
        }
        uint64_t addr = lo & ~0xFull;
        bool is64 = ((lo >> 1) & 3) == 2;
        if (is64 && b < 5) {
            addr |= uint64_t(pci::read32(d.bus, d.dev, d.fn, uint8_t(0x10 + (b + 1) * 4))) << 32;
        }
        kprintf("  BAR%d MMIO 0x%lx %s%s\n", b, addr, is64 ? "64-bit" : "32-bit", (lo & 8) ? " prefetch" : "");
        if (is64) b++;
    }
}

}  // namespace

// ---------------------------------------------------------------- entry

extern "C" void (*__init_array_start[])();
extern "C" void (*__init_array_end[])();

extern "C" [[noreturn]] void kmain() {
    for (auto* f = __init_array_start; f != __init_array_end; f++) (*f)();
    bool have_serial = serial::init();
    idt::init();

    if (!LIMINE_BASE_REVISION_SUPPORTED) cpu::halt_forever();

    const char* cmdline = cmdline_req.response ? cmdline_req.response->cmdline : "";
    char font[16] = "auto", scale_s[8] = "";
    kstr::cmdline_get(cmdline, "font", font, sizeof(font));
    kstr::cmdline_get(cmdline, "scale", scale_s, sizeof(scale_s));
    int scale = int(kstr::to_long(scale_s, 0));

    limine_framebuffer* fb = nullptr;
    if (fb_req.response && fb_req.response->framebuffer_count > 0) fb = fb_req.response->framebuffers[0];
    bool have_fb = con::init(fb, font, scale);

    // test=panic: deliberately fault to check the panic screen on real hardware.
    char test[16] = "";
    if (kstr::cmdline_get(cmdline, "test", test, sizeof(test)) && memcmp(test, "panic", 6) == 0) {
        kprintf("test=panic: touching an unmapped address...\n");
        *reinterpret_cast<volatile uint64_t*>(0x0000700000000000ull) = 1;
    }

    // Memory and firmware tables first: everything after this may allocate.
    mm::init(memmap_req.response, hhdm_req.response ? hhdm_req.response->offset : 0);
    bool have_acpi = acpi::init(rsdp_req.response ? uint64_t(rsdp_req.response->address) : 0);
    // UEFI runtime services (time, NVRAM, reset). efi=off skips them.
    {
        char efi_opt[8] = "";
        kstr::cmdline_get(cmdline, "efi", efi_opt, sizeof(efi_opt));
        auto* em = efi_mm_req.response;
        efi::init(efi_st_req.response ? uint64_t(efi_st_req.response->address) : 0, em ? em->memmap : nullptr,
                  em ? em->memmap_size : 0, em ? em->desc_size : 0, memcmp(efi_opt, "off", 4) != 0);
    }

    // ---- header (full width)
    con::set_color(con::White);
    kprintf("%s %s", OS_NAME, OS_VERSION);
    kcprintf(con::Dim, "   Phase 0 boot log   build %s\n", OS_BUILD);
    con::set_color(con::Default);

    char brand[49];
    cpu_brand(brand);
    const char* fw = "?";
    if (fw_req.response) {
        switch (fw_req.response->firmware_type) {
        case LIMINE_FIRMWARE_TYPE_UEFI64: fw = "UEFI x64"; break;
        case LIMINE_FIRMWARE_TYPE_UEFI32: fw = "UEFI ia32"; break;
        case LIMINE_FIRMWARE_TYPE_X86BIOS: fw = "BIOS"; break;
        }
    }
    kprintf("Boot:  %s %s, firmware %s, cmdline \"%s\"\n", bl_req.response ? bl_req.response->name : "?",
            bl_req.response ? bl_req.response->version : "?", fw, cmdline);
    kprintf("CPU:   %s, %d logical CPUs (ACPI MADT)\n", brand, acpi::info().n_cpus);
    if (have_fb) {
        const con::Info& ci = con::info();
        kprintf("GOP:   %lux%lu %ubpp pitch %lu at %p, %d framebuffer(s); console %dx%d font %dx%d x%d\n", ci.width,
                ci.height, ci.bpp, ci.pitch, fb->address, int(fb_req.response->framebuffer_count), ci.cols, ci.rows,
                ci.font_w, ci.font_h, ci.scale);
    } else {
        kprintf("GOP:   no usable framebuffer, serial only\n");
    }
    kprintf("HHDM:  0x%lx   ACPI RSDP: 0x%lx   Serial COM1: %s\n", hhdm_req.response ? hhdm_req.response->offset : 0,
            rsdp_req.response ? uint64_t(rsdp_req.response->address) : 0, have_serial ? "yes" : "not present");

    // ---- body: two panes side by side on wide screens, one pane otherwise
    const con::Info& ci = con::info();
    const int footer_rows = 14;
    const int title_row = con::cursor_row() + 1;
    const int body_top = title_row + 1;
    const int body_h = ci.rows - body_top - footer_rows - 1;
    int left = -1, right = -1, footer = -1;
    if (have_fb && ci.cols >= 150 && body_h >= 12) {
        const int lw = 64;
        int lt = con::add_pane(0, title_row, lw, 1);
        int rt = con::add_pane(lw + 2, title_row, ci.cols - lw - 2, 1);
        con::use_pane(lt);
        kcprintf(con::Cyan, "== Memory map (merged) ==");
        serial::putc('\n');
        con::use_pane(rt);
        kcprintf(con::Cyan, "== PCI devices: bus:dev.fn vendor:device class ==");
        serial::putc('\n');
        left = con::add_pane(0, body_top, lw, body_h);
        right = con::add_pane(lw + 2, body_top, ci.cols - lw - 2, body_h);
        con::draw_vline(lw, body_top, body_top + body_h - 1, con::Dim);
        footer = con::add_pane(0, body_top + body_h + 1, ci.cols, ci.rows - (body_top + body_h + 1));
    }

    const bool split = left >= 0;
    con::use_pane(left);
    if (!split) kprintf("\n");
    print_memmap(!split);

    con::use_pane(right);
    if (!split) kprintf("\n");
    print_pci(!split);

    // ---- footer: NPU + gate verdict
    con::use_pane(footer);
    if (!split) kprintf("\n");
    if (g_npu_index >= 0) {
        const pci::Device& d = pci::at(g_npu_index);
        kcprintf(con::Green, "NPU FOUND: %s at %02x:%02x.%x, id %04x:%04x rev %02x, subsystem %04x:%04x\n",
                 pci::intel_npu_name(d), d.bus, d.dev, d.fn, d.vendor, d.device, d.rev, d.subsys_vendor, d.subsys_id);
        print_bars(d);
    } else {
        kcprintf(con::Yellow, "NPU: no Intel NPU on the PCI bus (expected in QEMU; on AURORA look for 8086:ad1d at 00:0b.0)\n");
    }
    bool gate = have_fb && memmap_req.response && pci::count() > 0;
    if (gate && g_npu_index >= 0)
        kcprintf(con::Green, "Phase 0 gate: PASS (GOP log + memory map + PCI list + NPU)\n");
    else if (gate)
        kcprintf(con::Yellow, "Phase 0 gate: log OK, NPU missing\n");
    else
        kcprintf(con::Red, "Phase 0 gate: FAIL (framebuffer/memmap/PCI missing)\n");

    // ---- Phase 1 basics
    char sz[32];
    const acpi::Info& ai = acpi::info();
    if (have_acpi) {
        kprintf("ACPI:  rev %u OEM '%s', %d tables:", ai.revision, ai.oem, ai.n_tables);
        for (int i = 0; i < ai.n_tables; i++) kprintf(" %s", ai.tables[i]);
        kprintf("\n");
    } else {
        kcprintf(con::Red, "ACPI:  RSDP not usable\n");
    }
    bool have_apic = have_acpi && apic::init();
    if (have_apic) {
        apic::set_tick_hook(kbd::service);  // keyboard fallback if IRQ1 never fires
        apic::start_timer(100);
    }
    kprintf("IRQ:   LAPIC 0x%lx, %d IOAPIC, %d overrides; TSC %lu MHz (%s); timer %s\n", ai.lapic_addr, ai.n_ioapic,
            ai.n_iso, apic::tsc_hz() / 1000000, apic::tsc_source(), have_apic ? "100 Hz" : "OFF");
    if (efi::available()) {
        efi::Time et;
        kprintf("EFI:   %s", efi::status());
        if (efi::get_time(et))
            kprintf("; time %04u-%02u-%02u %02u:%02u:%02u", et.year, et.month, et.day, et.hour, et.minute, et.second);
        kprintf("\n");
    } else {
        kcprintf(con::Yellow, "EFI:   %s\n", efi::status());
    }
    bool have_kbd = kbd::init();
    kprintf("Input: i8042 %s%s\n", have_kbd ? "found" : "NOT found",
            ai.fadt_8042_flag_valid ? (ai.has_8042 ? " (FADT: 8042 present)" : " (FADT: no 8042)") : "");
    if (have_apic) cpu::sti();

    fs::init();
    int files = 0;
    if (module_req.response) {
        for (uint64_t i = 0; i < module_req.response->module_count; i++) {
            limine_file* m = module_req.response->modules[i];
            files += fs::load_tar(m->address, m->size, "/");
        }
    }
    efi::publish_files();
    // NPU bring-up, on by default when an NPU is present: power up, start the
    // firmware, power down. npu=power stops after the power test, npu=off skips it.
    {
        char npu_opt[8] = "";
        kstr::cmdline_get(cmdline, "npu", npu_opt, sizeof(npu_opt));
        if (g_npu_index >= 0 && memcmp(npu_opt, "off", 4) != 0)
            npu::bringup([](const char* l) { kprintf("%s\n", l); }, memcmp(npu_opt, "power", 6) != 0);
    }
    fs::put_file("/sys/bootlog.txt", con::log_data(), con::log_size());
    kprintf("FS:    RAM file system, %d entries from initrd (%s)\n", files, size_str(fs::used_bytes(), sz));
    kprintf("RAM:   %s managed, %s free\n", size_str(mm::total_bytes(), sz), size_str(mm::free_bytes(), sz));

    // ---- hand over to the file manager
    const int autostart_s = 15;
    kcprintf(con::White, "Enter: файловый менеджер (сам запустится через %d с)\n", autostart_s);
    con::flush();
    if (have_apic) {
        uint64_t until = apic::uptime_ms() + autostart_s * 1000ull;
        while (apic::uptime_ms() < until) {
            kbd::Event e;
            if (kbd::poll(e) && (e.key == kbd::Enter || e.key == kbd::Esc || e.key == kbd::Char)) break;
            cpu::sti_hlt();
        }
        char usb[16] = "", usbrw[16] = "";
        kstr::cmdline_get(cmdline, "usb", usb, sizeof(usb));
        kstr::cmdline_get(cmdline, "usbrw", usbrw, sizeof(usbrw));
        apps::g_usb_all_rw = memcmp(usbrw, "all", 4) == 0;
        apps::file_manager(memcmp(usb, "auto", 5) == 0);
    }
    kcprintf(con::Red, "No interrupt controller: cannot run the file manager. Halted.\n");
    con::flush();
    cpu::halt_forever();
}
