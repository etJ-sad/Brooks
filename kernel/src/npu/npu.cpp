// SPDX-License-Identifier: GPL-2.0-only
// Register sequences follow Linux drivers/accel/ivpu (Copyright (C) 2020-2024
// Intel Corporation, GPL-2.0-only): ivpu_hw_power_up(), ivpu_hw_power_down().
#include "npu/npu.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "npu/boot.h"
#include "npu/hw.h"
#include "pci/pci.h"

namespace npu {

namespace {
void (*g_log)(const char*) = nullptr;
char g_text[24576];
size_t g_len = 0;
}  // namespace

void log_line(const char* line) {
    size_t l = strlen(line);
    if (g_len + l + 2 < sizeof(g_text)) {
        memcpy(g_text + g_len, line, l);
        g_len += l;
        g_text[g_len++] = '\n';
    }
    if (g_log) g_log(line);
}

namespace {
using namespace hw;

// ---- buttress (BAR4), MTL generation (Meteor Lake, Arrow Lake)
constexpr uint32_t BTRS_WP_REQ_PAYLOAD0 = 0x08;  // MIN_RATIO 15:0, MAX_RATIO 31:16
constexpr uint32_t BTRS_WP_REQ_PAYLOAD1 = 0x0c;  // TARGET_RATIO 15:0, EPP 31:16
constexpr uint32_t BTRS_WP_REQ_PAYLOAD2 = 0x10;  // CONFIG 15:0
constexpr uint32_t BTRS_WP_REQ_CMD = 0x14;       // SEND bit 0
constexpr uint32_t BTRS_FMIN_FUSE = 0x24;        // MIN_RATIO 7:0, PN_RATIO 15:8
constexpr uint32_t BTRS_FMAX_FUSE = 0x28;        // MAX_RATIO 7:0
constexpr uint32_t BTRS_PLL_STATUS = 0x40;       // LOCK bit 1
constexpr uint32_t BTRS_VPU_STATUS = 0x44;       // READY bit 0, IDLE bit 1
constexpr uint32_t BTRS_VPU_IP_RESET = 0x50;     // TRIGGER bit 0
constexpr uint32_t BTRS_D0I3_CONTROL = 0x60;     // INPROGRESS bit 0, I3 bit 2

// ---- NPU IP (BAR0), 37xx generation
constexpr uint32_t HOST_SS_CPR_CLK_SET = 0x84;
constexpr uint32_t HOST_SS_CPR_RST_SET = 0x94;
constexpr uint32_t HOST_SS_CPR_RST_CLR = 0x98;  // AON bit 0
constexpr uint32_t CPR_TOP_NOC = 1u << 1, CPR_DSS_MAS = 1u << 10, CPR_MSS_MAS = 1u << 11;
constexpr uint32_t HOST_SS_NOC_QREQN = 0x154, HOST_SS_NOC_QACCEPTN = 0x158, HOST_SS_NOC_QDENY = 0x15c;  // TOP_SOCMMIO bit 0
constexpr uint32_t TOP_NOC_QREQN = 0x160, TOP_NOC_QACCEPTN = 0x164, TOP_NOC_QDENY = 0x168;  // CPU_CTRL bit 0, L2CACHE bit 1
constexpr uint32_t AON_PWR_ISO_EN0 = 0x30020, AON_PWR_ISLAND_EN0 = 0x30024, AON_PWR_ISLAND_TRICKLE_EN0 = 0x30028,
                   AON_PWR_ISLAND_STATUS0 = 0x3002c;  // MSS_CPU bit 3
constexpr uint32_t MSS_CPU = 1u << 3;
constexpr uint32_t AON_VPU_IDLE_GEN = 0x30200;  // EN bit 0
constexpr uint32_t AON_DPU_ACTIVE = 0x30204;    // DPU_ACTIVE bit 0
constexpr uint32_t CPU_SS_TIM_PERF_FREE_CNT = 0x06029000;

constexpr uint64_t PLL_TIMEOUT_US = 1500 * 1000, TIMEOUT_US = 150 * 1000, PWR_ISLAND_TIMEOUT_US = 5000;
constexpr uint32_t WP_CONFIG_2_TILE_4_3 = (0x02 << 8) | 0x02;

uint8_t g_min = 0, g_max = 0, g_pn = 0;

// Handshake check. Linux reads once right after the write; we allow up to
// 1 ms for the hardware to answer, which costs nothing when it is instant.
bool check(uint32_t reg, uint32_t mask, uint32_t want, const char* what) {
    for (int i = 0; i < 100; i++) {
        if (fld(rv(reg), mask) == want) return true;
        udelay(10);
    }
    uint32_t v = rv(reg);
    out("    FAILED: %s (IP+0x%x = 0x%08x, want %u)", what, reg, v, want);
    return false;
}

void dump(const char* when) {
    out("  [%s] VPU_STATUS=0x%08x PLL_STATUS=0x%08x D0I3=0x%08x ISLAND=0x%08x NOC_QACC=0x%08x TOP_QACC=0x%08x",
        when, rb(BTRS_VPU_STATUS), rb(BTRS_PLL_STATUS), rb(BTRS_D0I3_CONTROL), rv(AON_PWR_ISLAND_STATUS0),
        rv(HOST_SS_NOC_QACCEPTN), rv(TOP_NOC_QACCEPTN));
}

// ---- buttress
bool wp_drive(bool enable) {
    uint16_t target = enable ? g_pn : 0, cfg = enable ? WP_CONFIG_2_TILE_4_3 : 0;
    if (!poll(true, BTRS_WP_REQ_CMD, 1, 0, PLL_TIMEOUT_US, "workpoint SEND idle (before)")) return false;
    wb(BTRS_WP_REQ_PAYLOAD0, (rb(BTRS_WP_REQ_PAYLOAD0) & 0) | g_min | (uint32_t(g_max) << 16));
    wb(BTRS_WP_REQ_PAYLOAD1, target | (0x80u << 16));  // EPP default
    wb(BTRS_WP_REQ_PAYLOAD2, (rb(BTRS_WP_REQ_PAYLOAD2) & 0xFFFF0000u) | cfg);
    wb(BTRS_WP_REQ_CMD, rb(BTRS_WP_REQ_CMD) | 1);
    if (!poll(true, BTRS_WP_REQ_CMD, 1, 0, PLL_TIMEOUT_US, "workpoint SEND idle (after)")) return false;
    if (!poll(true, BTRS_PLL_STATUS, 1u << 1, enable, PLL_TIMEOUT_US, "PLL lock")) return false;
    return poll(true, BTRS_VPU_STATUS, 1, enable, PLL_TIMEOUT_US, "VPU_STATUS.READY");
}

bool d0i3_drive(bool enable) {
    if (!poll(true, BTRS_D0I3_CONTROL, 1, 0, TIMEOUT_US, "D0i3 not in progress (before)")) return false;
    uint32_t v = rb(BTRS_D0I3_CONTROL);
    wb(BTRS_D0I3_CONTROL, enable ? (v | (1u << 2)) : (v & ~(1u << 2)));
    bool ok = poll(true, BTRS_D0I3_CONTROL, 1, 0, TIMEOUT_US, "D0i3 not in progress (after)");
    if (enable) udelay(5);
    return ok;
}

bool ip_reset() {
    if (!poll(true, BTRS_VPU_IP_RESET, 1, 0, TIMEOUT_US, "IP reset TRIGGER clear (before)")) return false;
    wb(BTRS_VPU_IP_RESET, rb(BTRS_VPU_IP_RESET) | 1);
    return poll(true, BTRS_VPU_IP_RESET, 1, 0, TIMEOUT_US, "IP reset done");
}

// ---- IP
bool host_ss_configure() {
    if (!poll(false, HOST_SS_CPR_RST_CLR, 1, 0, 100, "NPU IP bar (RST_CLR.AON)")) return false;
    wv(HOST_SS_CPR_RST_CLR, CPR_TOP_NOC | CPR_DSS_MAS | CPR_MSS_MAS);
    return check(HOST_SS_NOC_QREQN, 1, 0, "HOST_SS NOC QREQN = 0") &&
           check(HOST_SS_NOC_QACCEPTN, 1, 0, "HOST_SS NOC QACCEPTN = 0") &&
           check(HOST_SS_NOC_QDENY, 1, 0, "HOST_SS NOC QDENY = 0");
}

bool pwr_domain_enable() {
    wv(AON_PWR_ISLAND_TRICKLE_EN0, rv(AON_PWR_ISLAND_TRICKLE_EN0) | MSS_CPU);
    udelay(1);
    wv(AON_PWR_ISLAND_EN0, rv(AON_PWR_ISLAND_EN0) | MSS_CPU);
    if (!poll(false, AON_PWR_ISLAND_STATUS0, MSS_CPU, 1, PWR_ISLAND_TIMEOUT_US, "power island status")) return false;
    if (!check(TOP_NOC_QREQN, 3, 0, "TOP NOC QREQN = 0")) return false;
    wv(HOST_SS_CPR_CLK_SET, rv(HOST_SS_CPR_CLK_SET) | CPR_TOP_NOC | CPR_DSS_MAS | CPR_MSS_MAS);
    wv(AON_PWR_ISO_EN0, rv(AON_PWR_ISO_EN0) & ~MSS_CPU);
    wv(HOST_SS_CPR_RST_SET, rv(HOST_SS_CPR_RST_SET) | CPR_TOP_NOC | CPR_DSS_MAS | CPR_MSS_MAS);
    wv(AON_DPU_ACTIVE, rv(AON_DPU_ACTIVE) | 1);
    return true;
}

bool host_ss_axi_enable() {
    wv(HOST_SS_NOC_QREQN, rv(HOST_SS_NOC_QREQN) | 1);
    return check(HOST_SS_NOC_QACCEPTN, 1, 1, "HOST_SS NOC QACCEPTN = 1") &&
           check(HOST_SS_NOC_QDENY, 1, 0, "HOST_SS NOC QDENY = 0");
}

bool top_noc_enable() {
    wv(TOP_NOC_QREQN, rv(TOP_NOC_QREQN) | 3);
    return check(TOP_NOC_QACCEPTN, 3, 3, "TOP NOC QACCEPTN = 1,1") && check(TOP_NOC_QDENY, 3, 0, "TOP NOC QDENY = 0");
}

#define STEP(name, expr)                         \
    do {                                         \
        bool ok_ = (expr);                       \
        out("  %-28s %s", name, ok_ ? "ok" : "FAILED"); \
        if (!ok_) return false;                  \
    } while (0)

bool power_up() {
    // 37xx workaround from Linux: a workpoint 0 request first (may fail, only a warning).
    out("  %-28s %s", "workpoint 0 (WA)", wp_drive(false) ? "ok" : "warn");
    out("  %-28s %s", "leave D0i3", d0i3_drive(false) ? "ok" : "warn");
    STEP("workpoint request (PLL on)", wp_drive(true));
    dump("PLL on");
    STEP("host subsystem configure", host_ss_configure());
    wv(AON_VPU_IDLE_GEN, rv(AON_VPU_IDLE_GEN) & ~1u);
    STEP("power domain enable", pwr_domain_enable());
    STEP("host AXI enable", host_ss_axi_enable());
    STEP("top NOC enable", top_noc_enable());
    return true;
}

bool power_down() {
    bool ok = true;
    bool r = ip_reset();
    out("  %-28s %s", "IP reset", r ? "ok" : "FAILED");
    ok &= r;
    r = wp_drive(false);
    out("  %-28s %s", "workpoint 0 (PLL off)", r ? "ok" : "FAILED");
    ok &= r;
    r = d0i3_drive(true);
    out("  %-28s %s", "enter D0i3", r ? "ok" : "FAILED");
    return ok && r;
}

int find_cap(const pci::Device& d, uint8_t id) {
    if (!(pci::read32(d.bus, d.dev, d.fn, 0x04) & (1u << 20))) return -1;  // no capability list
    uint8_t p = uint8_t(pci::read32(d.bus, d.dev, d.fn, 0x34) & 0xFC);
    for (int guard = 0; p && guard < 48; guard++) {
        uint32_t h = pci::read32(d.bus, d.dev, d.fn, p);
        if ((h & 0xFF) == id) return p;
        p = uint8_t((h >> 8) & 0xFC);
    }
    return -1;
}

Result run(bool start_fw) {
    Result r;
    const pci::Device* dev = nullptr;
    for (int i = 0; i < pci::count(); i++)
        if (pci::intel_npu_name(pci::at(i))) {
            dev = &pci::at(i);
            break;
        }
    if (!dev) {
        out("NPU: not on the PCI bus, nothing to do");
        return r;
    }
    out("NPU bring-up: %s %02x:%02x.%x id %04x:%04x rev %02x", pci::intel_npu_name(*dev), dev->bus,
        dev->dev, dev->fn, dev->vendor, dev->device, dev->rev);
    if (dev->device != 0x7d1d && dev->device != 0xad1d) {
        out("  only Meteor Lake / Arrow Lake (NPU 37xx) is supported so far");
        return r;
    }
    // PCI power state D0 (the firmware may leave the device in D3hot).
    int pm = find_cap(*dev, 0x01);
    if (pm >= 0) {
        uint32_t pmcsr = pci::read32(dev->bus, dev->dev, dev->fn, uint8_t(pm + 4));
        out("  PCI PM: D%u", pmcsr & 3);
        if (pmcsr & 3) {
            pci::write32(dev->bus, dev->dev, dev->fn, uint8_t(pm + 4), pmcsr & ~3u);
            udelay(10000);
            out("  PCI PM: now D%u", pci::read32(dev->bus, dev->dev, dev->fn, uint8_t(pm + 4)) & 3);
        }
    }
    pci::enable_mmio_dma(*dev);
    uint64_t bar0 = pci::bar_address(*dev, 0), bar4 = pci::bar_address(*dev, 4);
    out("  BAR0 (NPU IP) 0x%lx, BAR4 (buttress) 0x%lx", bar0, bar4);
    if (!bar0 || !bar4) {
        out("  BAR not assigned by the firmware");
        return r;
    }
    if (!map_bars(bar0, bar4)) {
        out("  cannot map the BARs");
        return r;
    }
    if (rb(BTRS_VPU_STATUS) == 0xFFFFFFFFu) {
        out("  buttress reads 0xffffffff: device does not respond");
        return r;
    }
    uint32_t fmin = rb(BTRS_FMIN_FUSE), fmax = rb(BTRS_FMAX_FUSE);
    g_min = uint8_t(fmin & 0xFF);
    g_pn = uint8_t((fmin >> 8) & 0xFF);
    g_max = uint8_t(fmax & 0xFF);
    if (g_pn < g_min) g_pn = g_min;
    if (g_pn > g_max) g_pn = g_max;
    out("  PLL ratios: min %u (%u MHz), nominal %u (%u MHz), max %u (%u MHz)", g_min, g_min * 100u / 3, g_pn,
        g_pn * 100u / 3, g_max, g_max * 100u / 3);
    dump("start");

    out("Power up (step A):");
    r.power_up = power_up();
    dump(r.power_up ? "powered" : "after failure");
    if (r.power_up) {
        // The NPU's own free-running timer counts only while the IP is powered.
        uint64_t t1 = rv64(CPU_SS_TIM_PERF_FREE_CNT);
        udelay(1000);
        uint64_t t2 = rv64(CPU_SS_TIM_PERF_FREE_CNT);
        out("  NPU timer: %lu -> %lu (+%lu in 1 ms)%s", t1, t2, t2 - t1, t2 > t1 ? "  NPU IS ALIVE" : "");
    }
    if (r.power_up && start_fw) {
        out("Firmware (step B):");
        boot::Device bd{dev->bus, dev->device, dev->rev, g_min, g_pn, g_max};
        r.fw_tried = true;
        r.fw_ready = boot::run(bd);
    }
    out("Power down:");
    r.power_down = power_down();
    dump("end");
    // The NPU can no longer touch its memory once the IP is reset.
    if (r.power_down) free_all();
    else if (allocated_bytes()) out("  keeping %lu KiB of NPU memory allocated (reset not confirmed)", uint64_t(allocated_bytes()) >> 10);
    out("NPU step A (power): %s", r.power_up && r.power_down ? "PASS" : r.power_up ? "power up OK, power down FAILED" : "FAILED");
    if (r.fw_tried) out("NPU step B (firmware): %s", r.fw_ready ? "PASS, firmware answered" : "FAILED");
    return r;
}

}  // namespace

Result bringup(void (*log)(const char*), bool start_fw) {
    g_log = log;
    g_len = 0;
    Result r = run(start_fw);
    fs::put_file("/sys/npu/bringup.txt", g_text, g_len);
    return r;
}

}  // namespace npu
