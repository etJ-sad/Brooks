// SPDX-License-Identifier: GPL-2.0-only
#include "acpi/acpi.h"
#include "arch/x86_64/cpu.h"
#include "lib/string.h"
#include "mm/vmm.h"

namespace acpi {
namespace {

struct [[gnu::packed]] Rsdp {
    char sig[8];
    uint8_t checksum;
    char oem[6];
    uint8_t revision;
    uint32_t rsdt;
    uint32_t length;
    uint64_t xsdt;
    uint8_t ext_checksum;
    uint8_t reserved[3];
};

struct [[gnu::packed]] Sdt {
    char sig[4];
    uint32_t length;
    uint8_t revision, checksum;
    char oem[6], oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
};

struct [[gnu::packed]] Gas {
    uint8_t space, bit_width, bit_offset, access;
    uint64_t addr;
};

struct [[gnu::packed]] Fadt {
    Sdt h;
    uint32_t facs, dsdt;
    uint8_t reserved0, pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable, acpi_disable, s4bios, pstate;
    uint32_t pm1a_evt, pm1b_evt, pm1a_cnt, pm1b_cnt, pm2_cnt, pm_tmr, gpe0, gpe1;
    uint8_t pm1_evt_len, pm1_cnt_len, pm2_cnt_len, pm_tmr_len, gpe0_len, gpe1_len, gpe1_base, cst_cnt;
    uint16_t p_lvl2, p_lvl3, flush_size, flush_stride;
    uint8_t duty_off, duty_width, day_alarm, mon_alarm, century;
    uint16_t iapc_boot_arch;
    uint8_t reserved1;
    uint32_t flags;
    Gas reset_reg;
    uint8_t reset_value;
    uint16_t arm_boot_arch;
    uint8_t minor;
    uint64_t x_facs, x_dsdt;
    Gas x_pm1a_evt, x_pm1b_evt, x_pm1a_cnt, x_pm1b_cnt;
};

void spin(int n) {
    for (int i = 0; i < n; i++) cpu::pause();
}

Info g_info;
const Sdt* g_tables[64];
int g_ntables = 0;
const Fadt* g_fadt = nullptr;

const Sdt* map_table(uint64_t phys) {
    auto* h = static_cast<const Sdt*>(vmm::map(phys, sizeof(Sdt), vmm::WriteBack));
    if (!h || h->length < sizeof(Sdt) || h->length > (16u << 20)) return nullptr;
    return static_cast<const Sdt*>(vmm::map(phys, h->length, vmm::WriteBack));
}

void parse_madt(const Sdt* t) {
    struct [[gnu::packed]] Madt {
        Sdt h;
        uint32_t lapic;
        uint32_t flags;
    };
    auto* m = reinterpret_cast<const Madt*>(t);
    g_info.lapic_addr = m->lapic;
    auto* p = reinterpret_cast<const uint8_t*>(m) + sizeof(Madt);
    auto* end = reinterpret_cast<const uint8_t*>(m) + m->h.length;
    while (p + 2 <= end && p[1] >= 2) {
        switch (p[0]) {
        case 0:  // processor local APIC
            if (p[4] & 3) g_info.n_cpus++;
            break;
        case 1:  // IOAPIC
            if (g_info.n_ioapic < 8) {
                IoApic& io = g_info.ioapic[g_info.n_ioapic++];
                io.id = p[2];
                memcpy(&io.addr, p + 4, 4);
                memcpy(&io.gsi_base, p + 8, 4);
            }
            break;
        case 2:  // interrupt source override
            if (g_info.n_iso < 16) {
                Override& o = g_info.iso[g_info.n_iso++];
                o.irq = p[3];
                memcpy(&o.gsi, p + 4, 4);
                memcpy(&o.flags, p + 8, 2);
            }
            break;
        case 5:  // 64-bit LAPIC address override
            memcpy(&g_info.lapic_addr, p + 4, 8);
            break;
        case 9:  // processor local x2APIC
            if (p[8] & 3) g_info.n_cpus++;
            break;
        }
        p += p[1];
    }
}

}  // namespace

bool init(uint64_t rsdp_phys) {
    if (!rsdp_phys) return false;
    auto* r = static_cast<const Rsdp*>(vmm::map(rsdp_phys, sizeof(Rsdp), vmm::WriteBack));
    if (!r || memcmp(r->sig, "RSD PTR ", 8) != 0) return false;
    g_info.revision = r->revision;
    memcpy(g_info.oem, r->oem, 6);
    bool x = r->revision >= 2 && r->xsdt;
    const Sdt* root = map_table(x ? r->xsdt : r->rsdt);
    if (!root) return false;
    size_t n = (root->length - sizeof(Sdt)) / (x ? 8 : 4);
    auto* ents = reinterpret_cast<const uint8_t*>(root) + sizeof(Sdt);
    for (size_t i = 0; i < n && g_ntables < 64; i++) {
        uint64_t a = 0;
        memcpy(&a, ents + i * (x ? 8 : 4), x ? 8 : 4);
        const Sdt* t = map_table(a);
        if (!t) continue;
        g_tables[g_ntables++] = t;
        if (g_info.n_tables < 48) {
            memcpy(g_info.tables[g_info.n_tables], t->sig, 4);
            g_info.tables[g_info.n_tables++][4] = 0;
        }
        if (memcmp(t->sig, "APIC", 4) == 0) parse_madt(t);
        if (memcmp(t->sig, "FACP", 4) == 0) {
            g_fadt = reinterpret_cast<const Fadt*>(t);
            if (t->revision >= 2 && t->length > 110) {
                g_info.fadt_8042_flag_valid = true;
                g_info.has_8042 = g_fadt->iapc_boot_arch & 2;
            }
        }
        if (memcmp(t->sig, "MCFG", 4) == 0 && t->length >= 44 + 16) {
            g_info.has_mcfg = true;
            memcpy(&g_info.ecam_base, reinterpret_cast<const uint8_t*>(t) + 44, 8);
        }
    }
    g_info.ok = true;
    return true;
}

const Info& info() { return g_info; }

const void* find(const char* sig) {
    for (int i = 0; i < g_ntables; i++)
        if (memcmp(g_tables[i]->sig, sig, 4) == 0) return g_tables[i];
    return nullptr;
}

void reboot() {
    cpu::cli();
    // 1. ACPI reset register (FADT flag RESET_REG_SUP, bit 10).
    if (g_fadt && g_fadt->h.length >= 129 && (g_fadt->flags & (1u << 10))) {
        const Gas& g = g_fadt->reset_reg;
        if (g.space == 1) cpu::outb(uint16_t(g.addr), g_fadt->reset_value);
        else if (g.space == 0) {
            auto* p = static_cast<volatile uint8_t*>(vmm::map(g.addr, 1, vmm::Uncached));
            if (p) *p = g_fadt->reset_value;
        }
    }
    spin(1000000);
    // 2. PCI reset control register (0xCF9): full reset.
    cpu::outb(0xCF9, 0x02);
    cpu::outb(0xCF9, 0x0E);
    spin(1000000);
    // 3. Keyboard controller pulse.
    cpu::outb(0x64, 0xFE);
    spin(1000000);
    // 4. Triple fault.
    struct [[gnu::packed]] {
        uint16_t l;
        uint64_t b;
    } null_idt{0, 0};
    asm volatile("lidt %0; int3" ::"m"(null_idt));
    cpu::halt_forever();
}

void poweroff() {
    if (!g_fadt) return;
    // Find SLP_TYPa for \_S5 by scanning the DSDT for the "_S5_" package
    // (common shortcut until a real AML interpreter is ported).
    uint64_t dsdt_phys = (g_fadt->h.length >= 148 && g_fadt->x_dsdt) ? g_fadt->x_dsdt : g_fadt->dsdt;
    const Sdt* dsdt = map_table(dsdt_phys);
    if (!dsdt) return;
    auto* p = reinterpret_cast<const uint8_t*>(dsdt) + sizeof(Sdt);
    auto* end = reinterpret_cast<const uint8_t*>(dsdt) + dsdt->length;
    for (; p + 12 < end; p++) {
        if (memcmp(p, "_S5_", 4) != 0) continue;
        if (!((p[-1] == 0x08) || (p[-2] == 0x08 && p[-1] == '\\'))) continue;  // NameOp
        const uint8_t* q = p + 4;
        if (*q != 0x12) continue;  // PackageOp
        q++;
        q += ((*q >> 6) & 3) + 1;  // PkgLength
        q++;                       // NumElements
        auto read_int = [](const uint8_t*& s) -> uint16_t {
            if (*s == 0x0A) { s++; return *s++; }  // BytePrefix
            if (*s == 0x00) { s++; return 0; }     // ZeroOp
            if (*s == 0x01) { s++; return 1; }     // OneOp
            return uint16_t(*s++);
        };
        uint16_t slp_a = read_int(q);
        uint16_t slp_b = read_int(q);
        cpu::cli();
        uint32_t a = g_fadt->pm1a_cnt, b = g_fadt->pm1b_cnt;
        if (a) cpu::outw(uint16_t(a), uint16_t((slp_a << 10) | (1 << 13)));
        if (b) cpu::outw(uint16_t(b), uint16_t((slp_b << 10) | (1 << 13)));
        spin(10000000);
        cpu::sti();
        return;
    }
}

}  // namespace acpi
