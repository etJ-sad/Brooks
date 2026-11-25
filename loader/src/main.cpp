// SPDX-License-Identifier: GPL-2.0-only
// Brooks Boot: UEFI loader that speaks the subset of the Limine boot protocol
// the kernel uses. Reads /boot/limine/limine.conf, shows a menu, loads the
// kernel ELF and modules through the firmware's file system drivers, builds
// page tables (higher half + HHDM), exits boot services and jumps.
#include <stdarg.h>
#include "efi.h"

#define LIMINE_API_REVISION 3
#include "../../kernel/include/limine.h"

extern "C" void enter_kernel(uint64_t cr3, uint64_t gdtr, uint64_t stack, uint64_t entry);

// ---------------------------------------------------------------- freestanding basics
extern "C" {
void* memcpy(void* d, const void* s, size_t n) {
    auto* a = static_cast<uint8_t*>(d);
    auto* b = static_cast<const uint8_t*>(s);
    for (size_t i = 0; i < n; i++) a[i] = b[i];
    return d;
}
void* memset(void* d, int c, size_t n) {
    auto* a = static_cast<uint8_t*>(d);
    for (size_t i = 0; i < n; i++) a[i] = uint8_t(c);
    return d;
}
int memcmp(const void* x, const void* y, size_t n) {
    auto* a = static_cast<const uint8_t*>(x);
    auto* b = static_cast<const uint8_t*>(y);
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
// clang may reference this for large stack frames on the Windows target
void __chkstk() {}
int _fltused = 0;
}

namespace {

constexpr uint64_t HHDM = 0xffff800000000000ull;
constexpr uint64_t PAGE = 4096;
const char* const kName = "Brooks Boot";
const char* const kVersion = "0.1";

EFI_SYSTEM_TABLE* ST;
EFI_BOOT_SERVICES* BS;
EFI_HANDLE g_image;
EFI_LOADED_IMAGE_PROTOCOL* g_li;

size_t slen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

bool guid_eq(const EFI_GUID& a, const EFI_GUID& b) { return memcmp(&a, &b, sizeof(EFI_GUID)) == 0; }

// ---------------------------------------------------------------- console output

void puts16(const CHAR16* s) { ST->ConOut->OutputString(ST->ConOut, s); }

void puts(const char* s) {
    CHAR16 buf[256];
    size_t o = 0;
    for (; *s; s++) {
        if (*s == '\n') {
            buf[o++] = '\r';
        }
        uint8_t b = uint8_t(*s);
        buf[o++] = b < 0x80 ? CHAR16(b) : CHAR16('?');
        if (b >= 0x80) while ((uint8_t(s[1]) & 0xC0) == 0x80) s++;
        if (o >= 250) {
            buf[o] = 0;
            puts16(buf);
            o = 0;
        }
    }
    buf[o] = 0;
    puts16(buf);
}

void vfmt(char* out, size_t cap, const char* f, va_list ap) {
    size_t o = 0;
    auto put = [&](char c) {
        if (o + 1 < cap) out[o++] = c;
    };
    for (; *f; f++) {
        if (*f != '%') { put(*f); continue; }
        f++;
        int width = 0;
        bool zero = false;
        if (*f == '0') { zero = true; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        bool lng = false;
        while (*f == 'l') { lng = true; f++; }
        char tmp[24];
        int n = 0;
        if (*f == 's') {
            const char* s = va_arg(ap, const char*);
            for (; s && *s; s++) put(*s);
            continue;
        }
        if (*f == 'c') { put(char(va_arg(ap, int))); continue; }
        uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
        unsigned base = (*f == 'x') ? 16 : 10;
        do { tmp[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
        while (n < width) tmp[n++] = zero ? '0' : ' ';
        while (n) put(tmp[--n]);
    }
    out[o] = 0;
}

void printf(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vfmt(buf, sizeof(buf), f, ap);
    va_end(ap);
    puts(buf);
}

[[noreturn]] void fatal(const char* msg) {
    ST->ConOut->SetAttribute(ST->ConOut, 0x0C);
    printf("\nBrooks Boot error: %s\nSystem halted. Power off with the power button.\n", msg);
    for (;;) asm volatile("hlt");
}

// ---------------------------------------------------------------- memory

uint64_t alloc_pages(uint64_t n, uint32_t type) {
    EFI_PHYSICAL_ADDRESS a = 0;
    if (BS->AllocatePages(AllocateAnyPages, type, n, &a) != EFI_SUCCESS) fatal("out of memory");
    memset(reinterpret_cast<void*>(a), 0, n * PAGE);
    return a;
}

// Bump arena for protocol responses (reported as bootloader-reclaimable).
uint8_t* g_arena;
size_t g_arena_used, g_arena_size;

void* arena(size_t n) {
    n = (n + 15) & ~size_t(15);
    if (g_arena_used + n > g_arena_size) fatal("response arena full");
    void* p = g_arena + g_arena_used;
    g_arena_used += n;
    return p;
}

template <typename T>
T* anew() { return static_cast<T*>(arena(sizeof(T))); }

uint64_t hv(const void* p) { return uint64_t(p) + HHDM; }  // HHDM virtual address

const char* astr(const char* s) {
    size_t l = slen(s);
    auto* p = static_cast<char*>(arena(l + 1));
    memcpy(p, s, l + 1);
    return p;
}

// ---------------------------------------------------------------- files

EFI_FILE_PROTOCOL* open_root(EFI_HANDLE h) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = nullptr;
    if (BS->HandleProtocol(h, &SIMPLE_FS_GUID, reinterpret_cast<void**>(&fs)) != EFI_SUCCESS || !fs) return nullptr;
    EFI_FILE_PROTOCOL* root = nullptr;
    if (fs->OpenVolume(fs, &root) != EFI_SUCCESS) return nullptr;
    return root;
}

// Loads "boot():/a/b" or "/a/b" from the boot volume, falling back to every
// other volume the firmware can read. Memory type selects how it is reported.
bool load_file(const char* path, uint32_t type, uint8_t** out, uint64_t* size) {
    const char* p = path;
    if (memcmp(p, "boot():", 7) == 0) p += 7;
    CHAR16 wpath[260];
    size_t o = 0;
    for (; *p && o < 258; p++) wpath[o++] = (*p == '/') ? '\\' : CHAR16(uint8_t(*p));
    wpath[o] = 0;

    EFI_HANDLE* handles = nullptr;
    uint64_t nh = 0;
    BS->LocateHandleBuffer(ByProtocol, &SIMPLE_FS_GUID, nullptr, &nh, &handles);
    for (int64_t i = -1; i < int64_t(nh); i++) {
        EFI_HANDLE h = i < 0 ? g_li->DeviceHandle : handles[i];
        if (i >= 0 && h == g_li->DeviceHandle) continue;
        EFI_FILE_PROTOCOL* root = open_root(h);
        if (!root) continue;
        EFI_FILE_PROTOCOL* f = nullptr;
        if (root->Open(root, &f, wpath, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
            root->Close(root);
            continue;
        }
        uint8_t ibuf[512];
        uint64_t isz = sizeof(ibuf);
        if (f->GetInfo(f, &FILE_INFO_GUID, &isz, ibuf) != EFI_SUCCESS) {
            f->Close(f);
            root->Close(root);
            continue;
        }
        uint64_t fsz = reinterpret_cast<EFI_FILE_INFO*>(ibuf)->FileSize;
        uint64_t pa = alloc_pages((fsz + PAGE - 1) / PAGE + 1, type);
        uint64_t rd = fsz;
        EFI_STATUS st = f->Read(f, &rd, reinterpret_cast<void*>(pa));
        f->Close(f);
        root->Close(root);
        if (st != EFI_SUCCESS || rd != fsz) fatal("file read error");
        *out = reinterpret_cast<uint8_t*>(pa);
        *size = fsz;
        if (handles) BS->FreePool(handles);
        return true;
    }
    if (handles) BS->FreePool(handles);
    return false;
}

// ---------------------------------------------------------------- config (limine.conf subset)

struct Entry {
    char name[64];
    char protocol[16];
    char path[128];
    char cmdline[256];
    char modules[4][128];
    int nmodules;
    uint32_t res_w, res_h;
};

Entry g_entries[16];
int g_nentries = 0;
int g_timeout = 3;
int g_default = 0;

void trim_copy(char* dst, size_t cap, const char* s, const char* e) {
    while (s < e && (*s == ' ' || *s == '\t')) s++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    size_t n = size_t(e - s) < cap - 1 ? size_t(e - s) : cap - 1;
    memcpy(dst, s, n);
    dst[n] = 0;
}

int atoi(const char* s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

void parse_config(const char* text, size_t len) {
    const char* end = text + len;
    Entry* cur = nullptr;
    for (const char* line = text; line < end;) {
        const char* nl = line;
        while (nl < end && *nl != '\n') nl++;
        const char* s = line;
        while (s < nl && (*s == ' ' || *s == '\t')) s++;
        if (s < nl && *s != '#') {
            if (*s == '/') {
                if (g_nentries < 16) {
                    cur = &g_entries[g_nentries++];
                    memset(cur, 0, sizeof(*cur));
                    trim_copy(cur->name, sizeof(cur->name), s + 1, nl);
                    memcpy(cur->protocol, "limine", 7);
                } else {
                    cur = nullptr;
                }
            } else {
                const char* colon = s;
                while (colon < nl && *colon != ':') colon++;
                if (colon < nl) {
                    char key[32], val[256];
                    trim_copy(key, sizeof(key), s, colon);
                    trim_copy(val, sizeof(val), colon + 1, nl);
                    auto is = [&](const char* k) { return slen(key) == slen(k) && memcmp(key, k, slen(k)) == 0; };
                    if (!cur) {
                        if (is("timeout")) g_timeout = atoi(val);
                        if (is("default_entry")) g_default = atoi(val) - 1;
                    } else if (is("protocol")) {
                        trim_copy(cur->protocol, sizeof(cur->protocol), val, val + slen(val));
                    } else if (is("path") || is("kernel_path") || is("image_path")) {
                        trim_copy(cur->path, sizeof(cur->path), val, val + slen(val));
                    } else if (is("cmdline") || is("kernel_cmdline")) {
                        trim_copy(cur->cmdline, sizeof(cur->cmdline), val, val + slen(val));
                    } else if (is("module_path") && cur->nmodules < 4) {
                        trim_copy(cur->modules[cur->nmodules++], 128, val, val + slen(val));
                    } else if (is("resolution")) {
                        cur->res_w = uint32_t(atoi(val));
                        const char* x = val;
                        while (*x && *x != 'x') x++;
                        if (*x) cur->res_h = uint32_t(atoi(x + 1));
                    }
                }
            }
        }
        line = nl + 1;
    }
    if (g_default < 0 || g_default >= g_nentries) g_default = 0;
}

// ---------------------------------------------------------------- menu

int menu() {
    if (g_nentries == 0) fatal("no entries in /boot/limine/limine.conf");
    int sel = g_default;
    int remaining = g_timeout * 10;  // in 100 ms steps
    bool counting = g_timeout > 0;
    if (g_timeout == 0) return sel;
    for (;;) {
        ST->ConOut->ClearScreen(ST->ConOut);
        ST->ConOut->SetAttribute(ST->ConOut, 0x0B);
        printf("  %s %s\n\n", kName, kVersion);
        for (int i = 0; i < g_nentries; i++) {
            ST->ConOut->SetAttribute(ST->ConOut, i == sel ? 0x70 : 0x07);
            printf("  %c %s  \n", i == sel ? '>' : ' ', g_entries[i].name);
        }
        ST->ConOut->SetAttribute(ST->ConOut, 0x08);
        printf("\n  Up/Down + Enter.");
        if (counting) printf("  Booting in %d s...", (remaining + 9) / 10);
        printf("\n");
        for (int t = 0; t < 10; t++) {
            EFI_INPUT_KEY k;
            if (ST->ConIn->ReadKeyStroke(ST->ConIn, &k) == EFI_SUCCESS) {
                counting = false;
                if (k.ScanCode == 1 && sel > 0) sel--;
                else if (k.ScanCode == 2 && sel + 1 < g_nentries) sel++;
                else if (k.UnicodeChar == '\r' || k.UnicodeChar == '\n') {
                    ST->ConOut->SetAttribute(ST->ConOut, 0x07);
                    return sel;
                }
                break;
            }
            BS->Stall(100000);
            if (counting && --remaining <= 0) {
                ST->ConOut->SetAttribute(ST->ConOut, 0x07);
                return sel;
            }
        }
    }
}

// ---------------------------------------------------------------- graphics

EFI_GRAPHICS_OUTPUT_PROTOCOL* g_gop;

void set_mode(uint32_t w, uint32_t h) {
    if (BS->LocateProtocol(&GOP_GUID, nullptr, reinterpret_cast<void**>(&g_gop)) != EFI_SUCCESS) {
        g_gop = nullptr;
        return;
    }
    if (!w || !h) return;  // keep the firmware's (native) mode
    for (uint32_t m = 0; m < g_gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info;
        uint64_t sz;
        if (g_gop->QueryMode(g_gop, m, &sz, &info) != EFI_SUCCESS) continue;
        if (info->HorizontalResolution == w && info->VerticalResolution == h && info->PixelFormat < 3) {
            g_gop->SetMode(g_gop, m);
            return;
        }
    }
}

void mask_shift(uint32_t mask, uint8_t& size, uint8_t& shift) {
    size = shift = 0;
    if (!mask) return;
    while (!(mask & 1)) { mask >>= 1; shift++; }
    while (mask & 1) { mask >>= 1; size++; }
}

// ---------------------------------------------------------------- paging

uint64_t* g_pml4;

uint64_t* next_table(uint64_t* t, int idx) {
    if (!(t[idx] & 1)) t[idx] = alloc_pages(1, MEM_RECLAIMABLE) | 3;
    return reinterpret_cast<uint64_t*>(t[idx] & 0x000ffffffffff000ull);
}

void map_4k(uint64_t va, uint64_t pa, uint64_t flags) {
    uint64_t* pdpt = next_table(g_pml4, int((va >> 39) & 511));
    uint64_t* pd = next_table(pdpt, int((va >> 30) & 511));
    uint64_t* pt = next_table(pd, int((va >> 21) & 511));
    pt[(va >> 12) & 511] = pa | flags;
}

void map_2m(uint64_t va, uint64_t pa, uint64_t flags) {
    uint64_t* pdpt = next_table(g_pml4, int((va >> 39) & 511));
    uint64_t* pd = next_table(pdpt, int((va >> 30) & 511));
    if (pd[(va >> 21) & 511] & 1) return;  // already mapped
    pd[(va >> 21) & 511] = pa | flags | (1 << 7);
}

void map_hhdm_range(uint64_t base, uint64_t end, uint64_t flags) {
    base &= ~0x1FFFFFull;
    end = (end + 0x1FFFFF) & ~0x1FFFFFull;
    for (uint64_t a = base; a < end; a += 0x200000) map_2m(HHDM + a, a, flags);
}

// ---------------------------------------------------------------- memory map

uint32_t limine_type(uint32_t t) {
    switch (t) {
    case EfiConventionalMemory:
    case EfiBootServicesCode:
    case EfiBootServicesData: return LIMINE_MEMMAP_USABLE;
    case EfiLoaderCode:
    case EfiLoaderData:
    case MEM_RECLAIMABLE: return LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE;
    case MEM_KERNEL: return LIMINE_MEMMAP_EXECUTABLE_AND_MODULES;
    case EfiACPIReclaimMemory: return LIMINE_MEMMAP_ACPI_RECLAIMABLE;
    case EfiACPIMemoryNVS: return LIMINE_MEMMAP_ACPI_NVS;
    case EfiUnusableMemory: return LIMINE_MEMMAP_BAD_MEMORY;
    case EfiMemoryMappedIO:
    case EfiMemoryMappedIOPortSpace: return 0xFFFFFFFF;  // not reported
    default: return LIMINE_MEMMAP_RESERVED;
    }
}

}  // namespace

// ---------------------------------------------------------------- entry point

extern "C" EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE* st) {
    ST = st;
    BS = st->BootServices;
    g_image = image;
    BS->SetWatchdogTimer(0, 0, 0, nullptr);
    BS->HandleProtocol(image, &LOADED_IMAGE_GUID, reinterpret_cast<void**>(&g_li));
    ST->ConOut->ClearScreen(ST->ConOut);

    g_arena_size = 1 << 20;
    g_arena = reinterpret_cast<uint8_t*>(alloc_pages(g_arena_size / PAGE, MEM_RECLAIMABLE));

    // Config
    uint8_t* cfg;
    uint64_t cfg_size;
    if (!load_file("/boot/limine/limine.conf", EfiLoaderData, &cfg, &cfg_size) &&
        !load_file("/boot/limine.conf", EfiLoaderData, &cfg, &cfg_size) &&
        !load_file("/limine.conf", EfiLoaderData, &cfg, &cfg_size))
        fatal("limine.conf not found");
    parse_config(reinterpret_cast<const char*>(cfg), cfg_size);
    Entry& e = g_entries[menu()];
    ST->ConOut->ClearScreen(ST->ConOut);
    printf("%s: booting \"%s\"\n", kName, e.name);

    // Chainload another EFI program (e.g. the Limine fallback).
    if (memcmp(e.protocol, "efi", 4) == 0 || memcmp(e.protocol, "efi_chainload", 14) == 0) {
        uint8_t* img;
        uint64_t isz;
        if (!load_file(e.path, EfiLoaderData, &img, &isz)) fatal("EFI image not found");
        EFI_HANDLE child = nullptr;
        if (BS->LoadImage(false, image, nullptr, img, isz, &child) != EFI_SUCCESS) fatal("LoadImage failed");
        EFI_LOADED_IMAGE_PROTOCOL* cli = nullptr;
        BS->HandleProtocol(child, &LOADED_IMAGE_GUID, reinterpret_cast<void**>(&cli));
        if (cli) cli->DeviceHandle = g_li->DeviceHandle;
        BS->StartImage(child, nullptr, nullptr);
        fatal("chainloaded image returned");
    }

    // Kernel ELF
    uint8_t* elf;
    uint64_t elf_size;
    if (!load_file(e.path, EfiLoaderData, &elf, &elf_size)) fatal("kernel not found");
    if (memcmp(elf, "\x7f" "ELF", 4) != 0 || elf[4] != 2 || *reinterpret_cast<uint16_t*>(elf + 18) != 62)
        fatal("kernel is not an x86_64 ELF64 file");
    uint64_t entry = *reinterpret_cast<uint64_t*>(elf + 24);
    uint64_t phoff = *reinterpret_cast<uint64_t*>(elf + 32);
    uint16_t phentsize = *reinterpret_cast<uint16_t*>(elf + 54), phnum = *reinterpret_cast<uint16_t*>(elf + 56);
    uint64_t vmin = ~0ull, vmax = 0;
    for (int i = 0; i < phnum; i++) {
        uint8_t* ph = elf + phoff + uint64_t(i) * phentsize;
        if (*reinterpret_cast<uint32_t*>(ph) != 1) continue;  // PT_LOAD
        uint64_t va = *reinterpret_cast<uint64_t*>(ph + 16), memsz = *reinterpret_cast<uint64_t*>(ph + 40);
        if (va < vmin) vmin = va;
        if (va + memsz > vmax) vmax = va + memsz;
    }
    vmin &= ~(PAGE - 1);
    vmax = (vmax + PAGE - 1) & ~(PAGE - 1);
    if (vmin == ~0ull || vmax <= vmin) fatal("kernel has no loadable segments");
    uint64_t kpages = (vmax - vmin) / PAGE;
    uint64_t kphys = alloc_pages(kpages, MEM_KERNEL);
    for (int i = 0; i < phnum; i++) {
        uint8_t* ph = elf + phoff + uint64_t(i) * phentsize;
        if (*reinterpret_cast<uint32_t*>(ph) != 1) continue;
        uint64_t off = *reinterpret_cast<uint64_t*>(ph + 8), va = *reinterpret_cast<uint64_t*>(ph + 16);
        uint64_t filesz = *reinterpret_cast<uint64_t*>(ph + 32);
        memcpy(reinterpret_cast<void*>(kphys + (va - vmin)), elf + off, filesz);
    }

    // Graphics mode (before reading the framebuffer parameters)
    set_mode(e.res_w, e.res_h);

    // Modules
    limine_file* mods[4];
    int nmods = 0;
    for (int i = 0; i < e.nmodules; i++) {
        uint8_t* m;
        uint64_t msz;
        if (!load_file(e.modules[i], MEM_KERNEL, &m, &msz)) {
            printf("warning: module %s not found\n", e.modules[i]);
            continue;
        }
        limine_file* f = anew<limine_file>();
        f->address = reinterpret_cast<void*>(hv(m));
        f->size = msz;
        f->path = reinterpret_cast<char*>(hv(astr(e.modules[i])));
        f->string = reinterpret_cast<char*>(hv(astr("")));
        mods[nmods++] = f;
    }

    // Firmware tables
    uint64_t rsdp = 0, smbios = 0;
    for (uint64_t i = 0; i < ST->NumberOfTableEntries; i++) {
        const EFI_CONFIGURATION_TABLE& t = ST->ConfigurationTable[i];
        if (guid_eq(t.VendorGuid, ACPI20_GUID)) rsdp = uint64_t(t.VendorTable);
        else if (!rsdp && guid_eq(t.VendorGuid, ACPI10_GUID)) rsdp = uint64_t(t.VendorTable);
        if (guid_eq(t.VendorGuid, SMBIOS3_GUID)) smbios = uint64_t(t.VendorTable);
    }
    (void)smbios;

    // Pre-allocate the memory map response (filled after the final GetMemoryMap).
    constexpr int MAXMAP = 512;
    auto* mm_entries = static_cast<limine_memmap_entry*>(arena(sizeof(limine_memmap_entry) * MAXMAP));
    auto** mm_ptrs = static_cast<limine_memmap_entry**>(arena(sizeof(void*) * MAXMAP));
    auto* mm_resp = anew<limine_memmap_response>();
    auto* em_resp = anew<limine_efi_memmap_response>();  // raw EFI map, for runtime services

    // Stack and GDT
    uint64_t stack = alloc_pages(16, MEM_RECLAIMABLE);
    uint64_t gdt_page = alloc_pages(1, MEM_RECLAIMABLE);
    auto* gdt = reinterpret_cast<uint64_t*>(gdt_page);
    gdt[0] = 0;
    gdt[1] = 0x00009a000000ffffull;  // 16-bit code
    gdt[2] = 0x000093000000ffffull;  // 16-bit data
    gdt[3] = 0x00cf9a000000ffffull;  // 32-bit code
    gdt[4] = 0x00cf93000000ffffull;  // 32-bit data
    gdt[5] = 0x00af9b000000ffffull;  // 64-bit code (0x28)
    gdt[6] = 0x00af93000000ffffull;  // 64-bit data (0x30)
    auto* gdtr = reinterpret_cast<uint8_t*>(gdt_page + 64);
    *reinterpret_cast<uint16_t*>(gdtr) = 7 * 8 - 1;
    *reinterpret_cast<uint64_t*>(gdtr + 2) = HHDM + gdt_page;

    // Framebuffer response
    limine_framebuffer_response* fb_resp = nullptr;
    uint64_t fb_base = 0, fb_size = 0;
    if (g_gop && g_gop->Mode && g_gop->Mode->Info->PixelFormat < 3) {
        auto* info = g_gop->Mode->Info;
        auto* fb = anew<limine_framebuffer>();
        memset(fb, 0, sizeof(*fb));
        fb_base = g_gop->Mode->FrameBufferBase;
        fb_size = g_gop->Mode->FrameBufferSize;
        fb->address = reinterpret_cast<void*>(HHDM + fb_base);
        fb->width = info->HorizontalResolution;
        fb->height = info->VerticalResolution;
        fb->pitch = uint64_t(info->PixelsPerScanLine) * 4;
        fb->bpp = 32;
        fb->memory_model = LIMINE_FRAMEBUFFER_RGB;
        if (info->PixelFormat == 0) {  // RGBX: red in the lowest byte
            fb->red_mask_size = fb->green_mask_size = fb->blue_mask_size = 8;
            fb->red_mask_shift = 0; fb->green_mask_shift = 8; fb->blue_mask_shift = 16;
        } else if (info->PixelFormat == 1) {  // BGRX
            fb->red_mask_size = fb->green_mask_size = fb->blue_mask_size = 8;
            fb->red_mask_shift = 16; fb->green_mask_shift = 8; fb->blue_mask_shift = 0;
        } else {
            mask_shift(info->RedMask, fb->red_mask_size, fb->red_mask_shift);
            mask_shift(info->GreenMask, fb->green_mask_size, fb->green_mask_shift);
            mask_shift(info->BlueMask, fb->blue_mask_size, fb->blue_mask_shift);
        }
        auto** list = static_cast<limine_framebuffer**>(arena(sizeof(void*)));
        list[0] = reinterpret_cast<limine_framebuffer*>(hv(fb));
        fb_resp = anew<limine_framebuffer_response>();
        fb_resp->revision = 0;
        fb_resp->framebuffer_count = 1;
        fb_resp->framebuffers = reinterpret_cast<limine_framebuffer**>(hv(list));
    }

    // Answer the kernel's requests.
    static const uint64_t magic[2] = {0xc7b1dd30df4c8b88ull, 0x0a82e883a194f07bull};
    auto* img = reinterpret_cast<uint64_t*>(kphys);
    uint64_t words = kpages * PAGE / 8;
    for (uint64_t i = 0; i + 4 <= words; i++) {
        if (img[i] == 0xf9562b2d5c95a6c8ull && img[i + 1] == 0x6a7b384944536bdcull) {
            img[i + 1] = 3;  // loaded base revision
            img[i + 2] = 0;  // supported
            continue;
        }
        if (img[i] != magic[0] || img[i + 1] != magic[1]) continue;
        uint64_t id2 = img[i + 2], id3 = img[i + 3];
        uint64_t* resp_slot = &img[i + 5];
        auto is = [&](const uint64_t (&id)[4]) { return id[2] == id2 && id[3] == id3; };
        static const uint64_t FB[4] = LIMINE_FRAMEBUFFER_REQUEST, MM[4] = LIMINE_MEMMAP_REQUEST,
                              HH[4] = LIMINE_HHDM_REQUEST, CL[4] = LIMINE_EXECUTABLE_CMDLINE_REQUEST,
                              BI[4] = LIMINE_BOOTLOADER_INFO_REQUEST, FW[4] = LIMINE_FIRMWARE_TYPE_REQUEST,
                              RS[4] = LIMINE_RSDP_REQUEST, MO[4] = LIMINE_MODULE_REQUEST,
                              ST_[4] = LIMINE_EFI_SYSTEM_TABLE_REQUEST, EA[4] = LIMINE_EXECUTABLE_ADDRESS_REQUEST,
                              PM[4] = LIMINE_PAGING_MODE_REQUEST, SS[4] = LIMINE_STACK_SIZE_REQUEST,
                              EM[4] = LIMINE_EFI_MEMMAP_REQUEST;
        void* r = nullptr;
        if (is(FB)) {
            r = fb_resp;
        } else if (is(MM)) {
            r = mm_resp;
        } else if (is(HH)) {
            auto* x = anew<limine_hhdm_response>();
            x->revision = 0;
            x->offset = HHDM;
            r = x;
        } else if (is(CL)) {
            auto* x = anew<limine_executable_cmdline_response>();
            x->revision = 0;
            x->cmdline = reinterpret_cast<char*>(hv(astr(e.cmdline)));
            r = x;
        } else if (is(BI)) {
            auto* x = anew<limine_bootloader_info_response>();
            x->revision = 0;
            x->name = reinterpret_cast<char*>(hv(astr(kName)));
            x->version = reinterpret_cast<char*>(hv(astr(kVersion)));
            r = x;
        } else if (is(FW)) {
            auto* x = anew<limine_firmware_type_response>();
            x->revision = 0;
            x->firmware_type = LIMINE_FIRMWARE_TYPE_UEFI64;
            r = x;
        } else if (is(RS) && rsdp) {
            auto* x = anew<limine_rsdp_response>();
            x->revision = 0;
            x->address = rsdp;  // physical (base revision 3)
            r = x;
        } else if (is(MO)) {
            auto* x = anew<limine_module_response>();
            auto** list = static_cast<limine_file**>(arena(sizeof(void*) * 4));
            for (int k = 0; k < nmods; k++) list[k] = reinterpret_cast<limine_file*>(hv(mods[k]));
            x->revision = 0;
            x->module_count = uint64_t(nmods);
            x->modules = reinterpret_cast<limine_file**>(hv(list));
            r = x;
        } else if (is(EM)) {
            r = em_resp;
        } else if (is(ST_)) {
            auto* x = anew<limine_efi_system_table_response>();
            x->revision = 0;
            x->address = uint64_t(ST);  // physical
            r = x;
        } else if (is(EA)) {
            auto* x = anew<limine_executable_address_response>();
            x->revision = 0;
            x->physical_base = kphys;
            x->virtual_base = vmin;
            r = x;
        } else if (is(PM)) {
            auto* x = anew<limine_paging_mode_response>();
            x->revision = 0;
            x->mode = LIMINE_PAGING_MODE_X86_64_4LVL;
            r = x;
        } else if (is(SS)) {
            auto* x = anew<limine_stack_size_response>();
            x->revision = 0;
            r = x;
        }
        if (r) *resp_slot = hv(r);
    }

    // Page tables: kernel, HHDM (first 4 GiB + RAM above), loader image identity.
    g_pml4 = reinterpret_cast<uint64_t*>(alloc_pages(1, MEM_RECLAIMABLE));
    for (uint64_t p = 0; p < kpages; p++) map_4k(vmin + p * PAGE, kphys + p * PAGE, 3);
    uint64_t li_base = uint64_t(g_li->ImageBase) & ~(PAGE - 1);
    uint64_t li_end = (uint64_t(g_li->ImageBase) + g_li->ImageSize + PAGE - 1) & ~(PAGE - 1);
    for (uint64_t a = li_base; a < li_end; a += PAGE) map_4k(a, a, 3);
    if (fb_size) map_hhdm_range(fb_base, fb_base + fb_size, 3 | (1 << 3));  // PWT -> PAT1 = WC
    map_hhdm_range(0, 0x100000000ull, 3);
    {
        uint64_t msz = 0, key, dsz;
        uint32_t dver;
        BS->GetMemoryMap(&msz, nullptr, &key, &dsz, &dver);
        msz += 64 * dsz;
        auto* buf = reinterpret_cast<uint8_t*>(alloc_pages((msz + PAGE - 1) / PAGE, EfiLoaderData));
        if (BS->GetMemoryMap(&msz, reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(buf), &key, &dsz, &dver) == EFI_SUCCESS) {
            for (uint64_t off = 0; off < msz; off += dsz) {
                auto* d = reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(buf + off);
                uint64_t end = d->PhysicalStart + d->NumberOfPages * PAGE;
                uint32_t t = limine_type(d->Type);
                if (end > 0x100000000ull && t != 0xFFFFFFFF && t != LIMINE_MEMMAP_RESERVED)
                    map_hhdm_range(d->PhysicalStart, end, 3);
            }
        }
    }

    // Final memory map + ExitBootServices (no allocations from here on).
    uint64_t map_cap = 64 * 1024;
    auto* map = reinterpret_cast<uint8_t*>(alloc_pages(map_cap / PAGE, MEM_RECLAIMABLE));
    printf("Starting kernel...\n");
    for (int attempt = 0;; attempt++) {
        uint64_t msz = map_cap, key = 0, dsz = 0;
        uint32_t dver = 0;
        if (BS->GetMemoryMap(&msz, reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(map), &key, &dsz, &dver) != EFI_SUCCESS)
            fatal("GetMemoryMap failed");
        // Convert: sort by address (insertion sort), drop MMIO, merge neighbours.
        int n = 0;
        for (uint64_t off = 0; off < msz && n < MAXMAP - 1; off += dsz) {
            auto* d = reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(map + off);
            uint32_t t = limine_type(d->Type);
            if (t == 0xFFFFFFFF || d->NumberOfPages == 0) continue;
            limine_memmap_entry ent{d->PhysicalStart, d->NumberOfPages * PAGE, t};
            int k = n++;
            while (k > 0 && mm_entries[k - 1].base > ent.base) {
                mm_entries[k] = mm_entries[k - 1];
                k--;
            }
            mm_entries[k] = ent;
        }
        if (fb_size && n < MAXMAP) {
            limine_memmap_entry ent{fb_base, (fb_size + PAGE - 1) & ~(PAGE - 1), LIMINE_MEMMAP_FRAMEBUFFER};
            int k = n++;
            while (k > 0 && mm_entries[k - 1].base > ent.base) {
                mm_entries[k] = mm_entries[k - 1];
                k--;
            }
            mm_entries[k] = ent;
        }
        int m = 0;
        for (int i = 0; i < n; i++) {
            if (m > 0 && mm_entries[m - 1].type == mm_entries[i].type &&
                mm_entries[m - 1].base + mm_entries[m - 1].length == mm_entries[i].base)
                mm_entries[m - 1].length += mm_entries[i].length;
            else
                mm_entries[m++] = mm_entries[i];
        }
        for (int i = 0; i < m; i++) mm_ptrs[i] = reinterpret_cast<limine_memmap_entry*>(hv(&mm_entries[i]));
        mm_resp->revision = 0;
        mm_resp->entry_count = uint64_t(m);
        mm_resp->entries = reinterpret_cast<limine_memmap_entry**>(hv(mm_ptrs));
        em_resp->revision = 0;
        em_resp->memmap = reinterpret_cast<void*>(hv(map));
        em_resp->memmap_size = msz;
        em_resp->desc_size = dsz;
        em_resp->desc_version = dver;
        if (BS->ExitBootServices(image, key) == EFI_SUCCESS) break;
        if (attempt > 4) fatal("ExitBootServices failed");
    }

    // PAT: entry 1 = write-combining (framebuffer), others as power-on default.
    uint64_t pat = 0x0007040600070106ull;
    asm volatile("wrmsr" ::"c"(0x277), "a"(uint32_t(pat)), "d"(uint32_t(pat >> 32)));
    enter_kernel(reinterpret_cast<uint64_t>(g_pml4), gdt_page + 64, HHDM + stack + 16 * PAGE, entry);
    for (;;) asm volatile("hlt");
}
