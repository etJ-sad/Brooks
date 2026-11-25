// SPDX-License-Identifier: GPL-2.0-only
// Register layout and sequences follow Linux drivers/accel/ivpu/ivpu_mmu.c and
// ivpu_mmu_context.c (Copyright (C) 2020-2024 Intel Corporation, GPL-2.0-only).
#include "npu/mmu.h"
#include "lib/string.h"
#include "mm/pmm.h"

namespace npu::mmu {
namespace {
using namespace hw;

constexpr uint32_t REG_IDR0 = 0x200000, REG_IDR1 = 0x200004, REG_IDR3 = 0x20000c, REG_IDR5 = 0x200014;
constexpr uint32_t REG_CR0 = 0x200020, REG_CR0ACK = 0x200024, REG_CR1 = 0x200028;
constexpr uint32_t REG_IRQ_CTRL = 0x200050, REG_IRQ_CTRLACK = 0x200054;
constexpr uint32_t REG_GERROR = 0x200060, REG_GERRORN = 0x200064;
constexpr uint32_t REG_STRTAB_BASE = 0x200080, REG_STRTAB_BASE_CFG = 0x200088;
constexpr uint32_t REG_CMDQ_BASE = 0x200090, REG_CMDQ_PROD = 0x200098, REG_CMDQ_CONS = 0x20009c;
constexpr uint32_t REG_EVTQ_BASE = 0x2000a0;
constexpr uint32_t REG_EVTQ_PROD_SEC = 0x2000a8 + 0x10000, REG_EVTQ_CONS_SEC = 0x2000ac + 0x10000;

constexpr uint32_t CR0_SMMUEN = 1u << 0, CR0_EVTQEN = 1u << 2, CR0_CMDQEN = 1u << 3, CR0_ATSCHK = 1u << 4;
constexpr uint32_t IRQ_GERROR_EN = 1u << 0, IRQ_EVTQ_EN = 1u << 2;

constexpr uint32_t CDTAB_ENT_SIZE = 64, CDTAB_COUNT_LOG2 = 8, CDTAB_COUNT = 1u << CDTAB_COUNT_LOG2;
constexpr uint32_t STRTAB_ENT_SIZE = 64, STRTAB_COUNT = 4, STRTAB_LOG2 = 2;
constexpr uint32_t Q_LOG2 = 4, Q_COUNT = 1u << Q_LOG2, Q_WRAP_MASK = (Q_COUNT << 1) - 1;
constexpr uint32_t CMD_SIZE = 16, EVT_SIZE = 32;

constexpr uint64_t Q_BASE_RWA = 1ull << 62, STRTAB_BASE_RA = 1ull << 62;
constexpr uint64_t ADDR_51_5 = ((1ull << 52) - 1) & ~31ull, ADDR_51_6 = ((1ull << 52) - 1) & ~63ull;
constexpr uint64_t ADDR_51_4 = ((1ull << 52) - 1) & ~15ull;

constexpr uint64_t CMD_CFGI_ALL = 0x4, CMD_TLBI_NSNH_ALL = 0x30, CMD_SYNC = 0x46;

// CR1: tables and queues inner shareable, write-back
constexpr uint32_t CR1_VAL = (3u << 10) | (1u << 8) | (1u << 6) | (3u << 4) | (1u << 2) | (1u << 0);

// Page table entry flags (ARM64 stage-1 format)
constexpr uint64_t E_VALID = 1ull << 0, E_TYPE_PAGE = 1ull << 1, E_LLC_COHERENT = 1ull << 2, E_USER = 1ull << 6,
                   E_AF = 1ull << 10, E_NG = 1ull << 11;
constexpr uint64_t E_TABLE = E_TYPE_PAGE | E_VALID;
constexpr uint64_t E_MAPPED = E_AF | E_USER | E_NG | E_TYPE_PAGE | E_VALID;
constexpr uint64_t E_ADDR = ((1ull << 48) - 1) & ~0xFFFull;

constexpr uint64_t MMU_REG_TIMEOUT_US = 10000, MMU_QUEUE_TIMEOUT_US = 100000;

Buf g_cdtab, g_strtab, g_cmdq, g_evtq;
uint32_t g_cmd_prod = 0;
uint64_t g_pgd_phys = 0, g_rsv_pgd_phys = 0;

// Every page-table page we allocate, so commit() can write them back.
constexpr size_t MAX_PT = 256;
uint64_t g_pt[MAX_PT];
size_t g_npt = 0;

uint64_t* table(uint64_t phys) { return mm::phys_to_virt<uint64_t>(phys); }

uint64_t new_table() {
    if (g_npt == MAX_PT) return 0;
    Buf b;
    if (!alloc(b, mm::PAGE)) return 0;
    g_pt[g_npt++] = b.phys;
    return b.phys;
}

// Returns the next-level table at index idx of `parent`, creating it if needed.
uint64_t ensure(uint64_t parent, unsigned idx) {
    uint64_t& e = table(parent)[idx];
    if (e & E_VALID) return e & E_ADDR;
    uint64_t t = new_table();
    if (t) e = t | E_TABLE;
    return t;
}

bool write_cr0(uint32_t v) {
    wv(REG_CR0, v);
    return poll(false, REG_CR0ACK, 0xFFFFFFFFu, v, MMU_REG_TIMEOUT_US, "MMU CR0ACK");
}

bool write_irq_ctrl(uint32_t v) {
    wv(REG_IRQ_CTRL, v);
    return poll(false, REG_IRQ_CTRLACK, 0xFFFFFFFFu, v, MMU_REG_TIMEOUT_US, "MMU IRQ_CTRLACK");
}

bool cmd(uint64_t d0, uint64_t d1) {
    uint32_t idx = g_cmd_prod & (Q_COUNT - 1);
    uint64_t* q = reinterpret_cast<uint64_t*>(g_cmdq.cpu);
    q[idx * 2] = d0;
    q[idx * 2 + 1] = d1;
    g_cmd_prod = (g_cmd_prod + 1) & Q_WRAP_MASK;
    return true;
}

const char* cmdq_err(uint32_t e) {
    switch (e) {
        case 0: return "none";
        case 1: return "illegal command";
        case 2: return "abort";
        case 3: return "ATC invalidation sync";
        default: return "unknown";
    }
}

bool sync() {
    cmd(CMD_SYNC, 0);
    flush(g_cmdq.cpu, g_cmdq.size);
    wv(REG_CMDQ_PROD, g_cmd_prod);
    if (poll(false, REG_CMDQ_CONS, 0x00FFFFFFu, g_cmd_prod, MMU_QUEUE_TIMEOUT_US, "MMU command queue consumed")) return true;
    uint32_t c = rv(REG_CMDQ_CONS);
    out("    MMU command queue error: %s (CONS=0x%08x)", cmdq_err((c >> 24) & 0x7F), c);
    return false;
}

void strtab_link_cd(uint32_t sid) {
    uint64_t* e = reinterpret_cast<uint64_t*>(g_strtab.cpu + sid * STRTAB_ENT_SIZE);
    uint64_t s0 = (5ull << 1)                       // CFG = stage 1 translate
                  | (uint64_t(CDTAB_COUNT_LOG2) << 59)  // S1CDMAX
                  | (0ull << 4)                      // S1FMT linear
                  | 1ull                             // V
                  | (g_cdtab.phys & ADDR_51_6);
    uint64_t s1 = 0                    // S1DSS terminate, S1CIR/S1COR non-cacheable, S1CSH non-shareable
                  | (2ull << 48)       // PRIVCFG unprivileged
                  | (2ull << 50)       // INSTCFG data
                  | (uint64_t(STRTAB_LOG2) << 13)  // CONT
                  | (1ull << 19)       // MEV
                  | (1ull << 27);      // S1STALLD
    e[1] = s1;
    e[0] = s0;
}

void cd_set(uint32_t ssid, uint64_t pgd, bool fault_on_access) {
    uint64_t* e = reinterpret_cast<uint64_t*>(g_cdtab.cpu + ssid * CDTAB_ENT_SIZE);
    uint64_t c0 = 16ull                 // T0SZ: 48-bit
                  | (5ull << 32)        // IPS 48-bit
                  | (uint64_t(ssid) << 48)  // ASID
                  | (1ull << 30)        // EPD1
                  | (1ull << 41)        // AA64
                  | (1ull << 45)        // R
                  | (1ull << 47);       // ASET
    if (fault_on_access) c0 |= 1ull << 46;  // A: generate memory faults on the NPU side
    c0 |= 1ull << 31;                       // V
    e[1] = pgd & ADDR_51_4;
    e[2] = 0;
    e[3] = 0x0000000000007444ull;  // MAIR
    e[0] = c0;
}

bool reset() {
    memset(g_cmdq.cpu, 0, g_cmdq.size);
    memset(g_evtq.cpu, 0, g_evtq.size);
    flush(g_cmdq.cpu, g_cmdq.size);
    flush(g_evtq.cpu, g_evtq.size);
    g_cmd_prod = 0;

    if (!write_cr0(0)) return false;
    wv(REG_CR1, CR1_VAL);
    wv64(REG_STRTAB_BASE, STRTAB_BASE_RA | (g_strtab.phys & ADDR_51_6));
    wv(REG_STRTAB_BASE_CFG, STRTAB_LOG2);
    wv64(REG_CMDQ_BASE, Q_BASE_RWA | (g_cmdq.phys & ADDR_51_5) | Q_LOG2);
    wv(REG_CMDQ_PROD, 0);
    wv(REG_CMDQ_CONS, 0);
    uint32_t v = CR0_CMDQEN;
    if (!write_cr0(v)) return false;
    cmd(CMD_CFGI_ALL, 0x1f);
    cmd(CMD_TLBI_NSNH_ALL, 0);
    if (!sync()) return false;
    wv64(REG_EVTQ_BASE, Q_BASE_RWA | (g_evtq.phys & ADDR_51_5) | Q_LOG2);
    wv(REG_EVTQ_PROD_SEC, 0);
    wv(REG_EVTQ_CONS_SEC, 0);
    v |= CR0_EVTQEN;
    if (!write_cr0(v)) return false;
    v |= CR0_ATSCHK;
    if (!write_cr0(v)) return false;
    if (!write_irq_ctrl(0) || !write_irq_ctrl(IRQ_EVTQ_EN | IRQ_GERROR_EN)) return false;
    v |= CR0_SMMUEN;
    return write_cr0(v);
}

const char* event_name(uint32_t op) {
    switch (op) {
        case 0x01: return "unsupported upstream transaction";
        case 0x02: return "bad stream id";
        case 0x03: return "STE fetch";
        case 0x04: return "bad STE";
        case 0x06: return "stream disabled";
        case 0x07: return "translation forbidden";
        case 0x08: return "bad substream id";
        case 0x09: return "CD fetch";
        case 0x0a: return "bad CD";
        case 0x0b: return "walk external abort";
        case 0x10: return "translation fault";
        case 0x11: return "address size fault";
        case 0x12: return "access fault";
        case 0x13: return "permission fault";
        case 0x20: return "TLB conflict";
        case 0x21: return "config conflict";
        case 0x25: return "VMS fetch";
        default: return "?";
    }
}

}  // namespace

bool init() {
    out("  MMU ID: IDR0=0x%08x IDR1=0x%08x IDR3=0x%08x IDR5=0x%08x (Linux expects 080f3e0f 0e739d18 0000003c 00040070)",
        rv(REG_IDR0), rv(REG_IDR1), rv(REG_IDR3), rv(REG_IDR5));
    if (!alloc(g_cdtab, CDTAB_COUNT * CDTAB_ENT_SIZE) || !alloc(g_strtab, STRTAB_COUNT * STRTAB_ENT_SIZE) ||
        !alloc(g_cmdq, Q_COUNT * CMD_SIZE) || !alloc(g_evtq, Q_COUNT * EVT_SIZE)) {
        out("  MMU: out of memory");
        return false;
    }
    g_npt = 0;
    g_pgd_phys = new_table();
    g_rsv_pgd_phys = new_table();
    if (!g_pgd_phys || !g_rsv_pgd_phys) return false;
    strtab_link_cd(0);
    strtab_link_cd(3);
    cd_set(0, g_pgd_phys, true);      // global context (firmware, IPC, logs)
    cd_set(1, g_rsv_pgd_phys, true);  // reserved context, deliberately empty
    flush(g_strtab.cpu, g_strtab.size);
    flush(g_cdtab.cpu, g_cdtab.size);
    out("  MMU tables: stream 0x%lx, CD 0x%lx, cmdq 0x%lx, evtq 0x%lx, root 0x%lx", g_strtab.phys, g_cdtab.phys,
        g_cmdq.phys, g_evtq.phys, g_pgd_phys);
    if (!reset()) {
        report();
        return false;
    }
    // ivpu_mmu_enable(): invalidate everything once more after the reset.
    cmd(CMD_CFGI_ALL, 0x1f);
    cmd(CMD_TLBI_NSNH_ALL, 0);
    return sync();
}

bool map(Buf& b, uint64_t vpu, bool snooped) {
    uint64_t prot = E_MAPPED | (snooped ? E_LLC_COHERENT : 0);
    for (uint64_t off = 0; off < b.size; off += mm::PAGE) {
        uint64_t va = vpu + off;
        uint64_t pud = ensure(g_pgd_phys, (va >> 39) & 511);
        uint64_t pmd = pud ? ensure(pud, (va >> 30) & 511) : 0;
        uint64_t pte = pmd ? ensure(pmd, (va >> 21) & 511) : 0;
        if (!pte) {
            out("  MMU: out of page-table pages");
            return false;
        }
        table(pte)[(va >> 12) & 511] = (b.phys + off) | prot;
    }
    b.vpu = vpu;
    return true;
}

bool commit() {
    for (size_t i = 0; i < g_npt; i++) flush(table(g_pt[i]), mm::PAGE);
    cmd(CMD_TLBI_NSNH_ALL, 0);
    return sync();
}

uint64_t translate(uint64_t va) {
    uint64_t t = g_pgd_phys;
    for (int shift = 39; shift >= 12; shift -= 9) {
        uint64_t e = table(t)[(va >> shift) & 511];
        if (!(e & E_VALID)) return 0;
        t = e & E_ADDR;
    }
    return t | (va & 0xFFF);
}

void report() {
    uint32_t ge = rv(REG_GERROR), gn = rv(REG_GERRORN);
    out("  MMU: CR0ACK=0x%x GERROR=0x%x GERRORN=0x%x CMDQ prod/cons=0x%x/0x%x", rv(REG_CR0ACK), ge, gn,
        rv(REG_CMDQ_PROD), rv(REG_CMDQ_CONS));
    if (!g_evtq.cpu) return;
    uint32_t prod = rv(REG_EVTQ_PROD_SEC), cons = rv(REG_EVTQ_CONS_SEC);
    flush(g_evtq.cpu, g_evtq.size);
    int shown = 0;
    while ((cons & Q_WRAP_MASK) != (prod & Q_WRAP_MASK) && shown < 8) {
        const uint32_t* e = reinterpret_cast<const uint32_t*>(g_evtq.cpu + (cons & (Q_COUNT - 1)) * EVT_SIZE);
        uint64_t addr = uint64_t(e[4]) | (uint64_t(e[5]) << 32);
        out("  MMU event: %s (0x%02x) ssid %u addr 0x%lx  [%08x %08x %08x %08x %08x %08x %08x %08x]",
            event_name(e[0] & 0xFF), e[0] & 0xFF, (e[0] >> 12) & 0xFFFFF, addr, e[0], e[1], e[2], e[3], e[4], e[5],
            e[6], e[7]);
        cons = (cons + 1) & Q_WRAP_MASK;
        shown++;
    }
    if (shown) wv(REG_EVTQ_CONS_SEC, cons);
    else out("  MMU: no events");
}

}  // namespace npu::mmu
