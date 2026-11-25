// SPDX-License-Identifier: GPL-2.0-only
// Structures and sequences follow Linux drivers/accel/ivpu (ivpu_fw.c,
// ivpu_hw_ip.c, ivpu_ipc.c, ivpu_drv.c, vpu_boot_api.h; Copyright (C)
// 2020-2024 Intel Corporation, GPL-2.0-only / MIT for vpu_boot_api.h).
#include "npu/boot.h"
#include <stddef.h>
#include "fs/vfs.h"
#include "lib/string.h"
#include "npu/hw.h"
#include "npu/mmu.h"

namespace npu::boot {

const char* const FW_PATHS[] = {
    "/lib/firmware/intel/vpu/vpu_37xx_v1.bin",
    "/usb0/vpu_37xx_v1.bin",
    "/usb1/vpu_37xx_v1.bin",
    nullptr,
};

namespace {
using namespace hw;

// ---- firmware file (vpu_boot_api.h)
#pragma pack(push, 4)
struct FwHeader {
    uint32_t header_version;
    uint32_t image_format;
    uint64_t image_load_address;
    uint32_t image_size;
    uint64_t entry_point;
    uint8_t vpu_version[32];
    uint32_t compression_type;
    uint64_t firmware_version_load_address;
    uint32_t firmware_version_size;
    uint64_t boot_params_load_address;
    uint32_t api_version[16];
    uint32_t runtime_size;
    uint32_t shave_nn_fw_size;
    uint32_t preemption_buffer_1_size, preemption_buffer_2_size;
    uint32_t preemption_buffer_1_max_size, preemption_buffer_2_max_size;
    uint32_t preemption_reserved[4];
    uint64_t ro_section_start_address;
    uint32_t ro_section_size;
    uint32_t reserved;
};

struct L2Cfg {
    uint8_t use, cfg;
};

struct BootParams {
    uint32_t magic, vpu_id, vpu_count, reserved_0[5];
    uint32_t frequency, reserved_1[12], perf_clk_frequency, reserved_2[42];
    uint64_t ipc_header_area_start;
    uint32_t ipc_header_area_size;
    uint64_t shared_region_base;
    uint32_t shared_region_size;
    uint64_t ipc_payload_area_start;
    uint32_t ipc_payload_area_size;
    uint64_t global_aliased_pio_base;
    uint32_t global_aliased_pio_size;
    uint32_t autoconfig;
    L2Cfg cache_defaults[2];
    uint32_t reserved_3[3];
    uint64_t shave_nn_fw_base;
    uint64_t save_restore_ret_address;
    uint32_t reserved_4[43];
    int32_t watchdog_irq_mss, watchdog_irq_nce;
    uint32_t host_to_vpu_irq, job_done_irq, reserved_5[60];
    uint32_t host_version_id, si_stepping;
    uint64_t device_id, feature_exclusion, sku;
    uint32_t min_freq_pll_ratio, max_freq_pll_ratio, default_trace_level, boot_type;
    uint64_t punit_telemetry_sram_base, punit_telemetry_sram_size;
    uint32_t vpu_telemetry_enable;
    uint64_t crit_tracing_buff_addr;
    uint32_t crit_tracing_buff_size;
    uint64_t verbose_tracing_buff_addr;
    uint32_t verbose_tracing_buff_size;
    uint64_t verbose_tracing_sw_component_mask;
    uint32_t trace_destination_mask;
    uint64_t trace_hw_component_mask;
    uint64_t tracing_buff_message_format_mask;
    uint64_t trace_reserved_1[2];
    uint32_t reserved_6, pn_freq_pll_ratio, dvfs_mode;
    uint64_t dvfs_param;
    uint32_t d0i3_delayed_entry;
    uint64_t d0i3_residency_time_us, d0i3_entry_vpu_ts, system_time_us;
    uint32_t reserved_7[2];
    uint64_t device_time_delta_ticks;
    uint32_t reserved_8[30];
    uint64_t power_states_timestamps[6];
    uint32_t vpu_scheduling_mode, vpu_focus_present_timer_ms, vpu_uses_ecc_mca_signal, power_profile;
    uint32_t dct_active_us, dct_inactive_us;
    uint32_t reserved_9[734];
};
#pragma pack(pop)

static_assert(offsetof(FwHeader, boot_params_load_address) == 76);
static_assert(offsetof(FwHeader, runtime_size) == 148);
static_assert(offsetof(BootParams, frequency) == 0x20);
static_assert(offsetof(BootParams, ipc_header_area_start) == 0x100);
static_assert(offsetof(BootParams, watchdog_irq_mss) == 0x200);
static_assert(offsetof(BootParams, host_version_id) == 0x300);
static_assert(sizeof(BootParams) <= 4096);

struct IpcHdr {
    uint32_t data_addr, data_size;
    uint16_t channel;
    uint8_t src_node, dst_node, status;
};

constexpr uint32_t FW_HEADER_SIZE = 4096, FW_VERSION_HEADER_SIZE = 4096;
constexpr uint32_t FW_IMAGE_OFFSET = FW_HEADER_SIZE + FW_VERSION_HEADER_SIZE;
constexpr uint32_t BOOT_PARAMS_MAGIC = 0x10000;
constexpr uint32_t BOOT_API_MAJOR = 3, JSM_API_MAJOR = 3;
constexpr uint32_t BOOT_API_INDEX = 0, JSM_API_INDEX = 4;

// NPU address ranges for the 37xx IP (ivpu_hw.c memory_ranges_init)
constexpr uint64_t RUNTIME_START = 0x84800000, RUNTIME_SIZE = 64ull << 20;
constexpr uint64_t GLOBAL_START = 0x90000000, GLOBAL_SIZE = 256ull << 20;
constexpr uint64_t USER_START = 0xa0000000, USER_SIZE = 511ull << 20;
constexpr uint64_t SHAVE_START = 0x180000000;

constexpr uint32_t IPC_CHAN_BOOT_MSG = 0x3ff, IPC_BOOT_MSG_DATA_ADDR = 0x424f4f54;  // "BOOT"
constexpr uint64_t BOOT_TIMEOUT_US = 1000 * 1000;
constexpr uint32_t LOG_INFO = 2, TRACE_DEST_VERBOSE = 0x2;
constexpr uint32_t PLL_PROFILING_FREQ_DEFAULT = 38400000;

// ---- registers (ivpu_hw_ip_reg.h, 37xx)
constexpr uint32_t HOST_SS_ICB_STATUS_0 = 0x10210, HOST_SS_ICB_STATUS_1 = 0x10214;
constexpr uint32_t HOST_SS_TIM_IPC_FIFO_ATM = 0x200f4, HOST_SS_TIM_IPC_FIFO_STAT = 0x200fc;
constexpr uint32_t FIFO_FILL_LEVEL = 0x00FF0000u;
constexpr uint32_t HOST_SS_LOADING_ADDRESS_LO = 0x41040;  // DONE bit 0, IMAGE_LOCATION 31:3
constexpr uint32_t HOST_IF_TCU_PTW_OVERRIDES = 0x360000;
constexpr uint32_t PTW_NOSNOOP_OVERRIDE_EN = 1u << 3, PTW_AW_NOSNOOP = 1u << 4, PTW_AR_NOSNOOP = 1u << 5;
constexpr uint32_t HOST_IF_TBU_MMUSSIDV = 0x360004;
constexpr uint32_t TBU0_AW = 1u << 0, TBU0_AR = 1u << 1, TBU2_AW = 1u << 4, TBU2_AR = 1u << 5;
constexpr uint32_t CPU_SS_LEON_RT_VEC = 0x06010040;
constexpr uint32_t RT_RSTRUN0 = 1u << 0, RT_RESUME0 = 1u << 1, RT_RSTVEC = 0xFFFFFFF0u;
// buttress telemetry (MTL)
constexpr uint32_t BTRS_TELEMETRY_OFFSET = 0x80, BTRS_TELEMETRY_SIZE = 0x84, BTRS_TELEMETRY_ENABLE = 0x88;

Buf g_bp, g_ver, g_rt, g_shave, g_tx, g_rx, g_log_crit, g_log_verb;
uint64_t g_global_next = 0;

bool alloc_global(Buf& b, size_t size, bool snooped, const char* what) {
    if (!alloc(b, size)) {
        out("  out of memory for %s (%lu KiB)", what, uint64_t(size) >> 10);
        return false;
    }
    if (g_global_next + b.size > GLOBAL_START + GLOBAL_SIZE) return false;
    if (!mmu::map(b, g_global_next, snooped)) return false;
    g_global_next += b.size;
    return true;
}

bool alloc_at(Buf& b, uint64_t vpu, size_t size, const char* what) {
    if (!alloc(b, size)) {
        out("  out of memory for %s (%lu KiB)", what, uint64_t(size) >> 10);
        return false;
    }
    return mmu::map(b, vpu, false);
}

bool in_range(uint64_t a, uint64_t size, uint64_t start, uint64_t rsize) {
    return a >= start && a + size >= a && a + size <= start + rsize;
}

const fs::Node* find_firmware(const char** path) {
    for (int i = 0; FW_PATHS[i]; i++) {
        fs::Node* n = fs::lookup(FW_PATHS[i]);
        if (n && !n->dir && fs::ensure_data(n) && n->size > FW_IMAGE_OFFSET) {
            *path = FW_PATHS[i];
            return n;
        }
    }
    return nullptr;
}

struct Fw {
    const FwHeader* h;
    const uint8_t* data;
    size_t size;
    uint64_t runtime_addr, runtime_size, image_load_offset;
};

bool parse(const fs::Node* f, Fw& fw) {
    fw.data = f->data;
    fw.size = f->size;
    fw.h = reinterpret_cast<const FwHeader*>(f->data);
    const FwHeader& h = *fw.h;
    char ver[80];
    size_t vl = 0;
    for (const uint8_t* p = f->data + FW_HEADER_SIZE; vl + 1 < sizeof(ver) && *p >= 0x20 && *p < 0x7F; p++) ver[vl++] = char(*p);
    ver[vl] = 0;
    out("  firmware: %lu bytes, header v%u, version \"%s\"", uint64_t(fw.size), h.header_version, ver);
    out("  boot API %u.%u, JSM API %u.%u (driver built for boot 3.x, JSM 3.x)", h.api_version[BOOT_API_INDEX] >> 16,
        h.api_version[BOOT_API_INDEX] & 0xFFFF, h.api_version[JSM_API_INDEX] >> 16, h.api_version[JSM_API_INDEX] & 0xFFFF);
    if (h.header_version != 1) {
        out("  unknown firmware header version");
        return false;
    }
    if ((h.api_version[BOOT_API_INDEX] >> 16) != BOOT_API_MAJOR || (h.api_version[JSM_API_INDEX] >> 16) != JSM_API_MAJOR) {
        out("  firmware API major version does not match");
        return false;
    }
    fw.runtime_addr = h.image_load_address;
    fw.runtime_size = uint64_t(h.runtime_size) - 4096 - 4096;
    fw.image_load_offset = 0;
    out("  boot params @0x%lx, version @0x%lx, runtime @0x%lx size %lu KiB, image %u bytes, entry 0x%lx, SHAVE-NN %u",
        h.boot_params_load_address, h.firmware_version_load_address, fw.runtime_addr, fw.runtime_size >> 10,
        h.image_size, h.entry_point, h.shave_nn_fw_size);
    bool ok = in_range(h.boot_params_load_address, 4096, RUNTIME_START, RUNTIME_SIZE) &&
              h.firmware_version_size <= 4096 &&
              in_range(h.firmware_version_load_address, 4096, RUNTIME_START, RUNTIME_SIZE) &&
              h.runtime_size > 8192 && in_range(fw.runtime_addr, fw.runtime_size, RUNTIME_START, RUNTIME_SIZE) &&
              uint64_t(FW_IMAGE_OFFSET) + h.image_size <= fw.size && (fw.runtime_addr & 0xFFF) == 0 &&
              (fw.runtime_size & 0xFFF) == 0 && fw.runtime_size >= h.image_size &&
              h.entry_point >= h.image_load_address && h.entry_point + 4096 <= h.image_load_address + h.image_size &&
              h.shave_nn_fw_size <= (2u << 20);
    if (!ok) out("  firmware header fails the Linux sanity checks");
    return ok;
}

void dump_state(const char* when) {
    out("  [%s] FIFO_STAT=0x%08x ICB_STATUS=0x%08x/0x%08x LEON_RT_VEC=0x%08x LOADING_ADDRESS=0x%08x", when,
        rv(HOST_SS_TIM_IPC_FIFO_STAT), rv(HOST_SS_ICB_STATUS_0), rv(HOST_SS_ICB_STATUS_1), rv(CPU_SS_LEON_RT_VEC),
        rv(HOST_SS_LOADING_ADDRESS_LO));
}

// Firmware trace buffer: 128-byte header (vpu_tracing_buffer_header), then text.
void save_log(const Buf& b, const char* name, const char* path, int tail_lines) {
    flush(b.cpu, b.size);
    const uint32_t* w = reinterpret_cast<const uint32_t*>(b.cpu);
    uint32_t write_index = w[17], wraps = w[18], size = w[20];
    uint16_t hdr = *reinterpret_cast<const uint16_t*>(b.cpu + 86);
    if (hdr < 128 || hdr >= b.size) hdr = 128;
    if (size == 0 || size > b.size) size = uint32_t(b.size);
    uint32_t data_len = size > hdr ? size - hdr : 0;
    // write_index counts from the buffer start in newer firmware, from the data start in older.
    uint32_t end = write_index >= hdr && write_index <= size ? write_index - hdr : write_index;
    if (end > data_len) end = data_len;
    out("  %s log: canary 0x%08x, write index %u, wraps %u, %u bytes", name, w[16], write_index, wraps, end);
    const char* text = reinterpret_cast<const char*>(b.cpu + hdr);
    fs::put_file(path, text, end);
    if (tail_lines <= 0 || !end) return;
    // Print the last lines, printable characters only.
    uint32_t start = end, lines = 0;
    while (start > 0 && lines <= uint32_t(tail_lines)) {
        if (text[start - 1] == '\n') lines++;
        if (lines > uint32_t(tail_lines)) break;
        start--;
    }
    char line[160];
    size_t n = 0;
    for (uint32_t i = start; i <= end; i++) {
        char c = i < end ? text[i] : '\n';
        if (c == '\n' || n + 1 == sizeof(line)) {
            line[n] = 0;
            if (n) out("    fw| %s", line);
            n = 0;
        } else if (c >= 0x20 && c < 0x7F) {
            line[n++] = c;
        }
    }
}

bool wait_for_ready() {
    uint64_t waited = 0;
    while (!fld(rv(HOST_SS_TIM_IPC_FIFO_STAT), FIFO_FILL_LEVEL)) {
        if (waited >= BOOT_TIMEOUT_US) {
            out("  no message from the firmware after %lu ms", waited / 1000);
            return false;
        }
        udelay(100);
        waited += 100;
    }
    out("  first IPC message after %lu ms", waited / 1000);
    bool ok = false;
    for (int guard = 0; guard < 16 && fld(rv(HOST_SS_TIM_IPC_FIFO_STAT), FIFO_FILL_LEVEL); guard++) {
        uint32_t a = rv(HOST_SS_TIM_IPC_FIFO_ATM);
        if (a < g_rx.vpu || a + sizeof(IpcHdr) > g_rx.vpu + g_rx.size) {
            out("  IPC message at 0x%08x is outside the receive buffer", a);
            continue;
        }
        uint8_t* p = g_rx.cpu + (a - g_rx.vpu);
        flush(p, 64);
        IpcHdr h;
        memcpy(&h, p, sizeof(h));
        out("  IPC rx @0x%08x: channel 0x%x, data_addr 0x%08x, size %u, status %u", a, h.channel, h.data_addr,
            h.data_size, h.status);
        if (h.channel == IPC_CHAN_BOOT_MSG) {
            if (h.data_addr == IPC_BOOT_MSG_DATA_ADDR) ok = true;
            else out("  invalid ready message 0x%08x", h.data_addr);
        }
        reinterpret_cast<IpcHdr*>(p)->status = 0;  // IVPU_IPC_HDR_FREE
        flush(p, 64);
    }
    return ok;
}

}  // namespace

bool run(const Device& dev) {
    const char* path = nullptr;
    const fs::Node* f = find_firmware(&path);
    if (!f) {
        out("  firmware vpu_37xx_v1.bin not found (looked in %s, /usb0, /usb1)", FW_PATHS[0]);
        return false;
    }
    out("  firmware file: %s", path);
    Fw fw;
    if (!parse(f, fw)) return false;
    const FwHeader& h = *fw.h;

    out("NPU MMU:");
    if (!mmu::init()) {
        out("  MMU enable FAILED");
        return false;
    }
    out("  MMU enabled");

    // Runtime area: boot params, version page, then the firmware runtime with the image.
    g_global_next = GLOBAL_START;
    if (!alloc_at(g_bp, h.boot_params_load_address, 4096, "boot params") ||
        !alloc_at(g_ver, h.firmware_version_load_address, 4096, "version page") ||
        !alloc_at(g_rt, fw.runtime_addr, fw.runtime_size, "firmware runtime"))
        return false;
    uint64_t shave_size = (uint64_t(h.shave_nn_fw_size) + 4095) & ~4095ull;
    if (shave_size && !alloc_at(g_shave, SHAVE_START, shave_size, "SHAVE-NN")) return false;
    if (!alloc_global(g_tx, 16 << 10, false, "IPC tx") || !alloc_global(g_rx, 16 << 10, false, "IPC rx") ||
        !alloc_global(g_log_crit, 512 << 10, true, "critical log") ||
        !alloc_global(g_log_verb, 1 << 20, true, "verbose log"))
        return false;
    out("  mapped: runtime 0x%lx..0x%lx, IPC tx 0x%lx rx 0x%lx, logs 0x%lx 0x%lx; %lu KiB of RAM in use",
        g_bp.vpu, g_rt.vpu + g_rt.size, g_tx.vpu, g_rx.vpu, g_log_crit.vpu, g_log_verb.vpu,
        uint64_t(allocated_bytes()) >> 10);

    // ivpu_fw_load(): image at its load offset, zeroes around it.
    memcpy(g_rt.cpu + fw.image_load_offset, fw.data + FW_IMAGE_OFFSET, h.image_size);
    flush(g_rt.cpu, g_rt.size);
    uint64_t entry_phys = mmu::translate(h.entry_point);
    uint64_t want_phys = g_rt.phys + (h.entry_point - fw.runtime_addr);
    out("  image copied; entry 0x%lx -> phys 0x%lx (%s)", h.entry_point, entry_phys,
        entry_phys == want_phys ? "page tables ok" : "PAGE TABLE MISMATCH");
    if (entry_phys != want_phys) return false;

    // ivpu_fw_boot_params_setup(), cold boot.
    BootParams* bp = reinterpret_cast<BootParams*>(g_bp.cpu);
    memset(bp, 0, sizeof(*bp));
    bp->boot_type = 0;  // cold
    bp->magic = BOOT_PARAMS_MAGIC;
    bp->vpu_id = dev.bus;
    bp->perf_clk_frequency = PLL_PROFILING_FREQ_DEFAULT;
    bp->shared_region_base = GLOBAL_START;
    bp->shared_region_size = uint32_t(GLOBAL_SIZE);
    bp->ipc_header_area_start = g_rx.vpu;
    bp->ipc_header_area_size = uint32_t(g_rx.size / 2);
    bp->ipc_payload_area_start = g_rx.vpu + g_rx.size / 2;
    bp->ipc_payload_area_size = uint32_t(g_rx.size / 2);
    bp->global_aliased_pio_base = USER_START;
    bp->global_aliased_pio_size = uint32_t(USER_SIZE);
    bp->autoconfig = 1;
    bp->cache_defaults[1].use = 1;  // NN: L2 cache for the 2 GiB SHAVE window
    bp->cache_defaults[1].cfg = uint8_t(SHAVE_START >> 31);
    bp->shave_nn_fw_base = g_shave.vpu;
    bp->watchdog_irq_mss = 32;
    bp->watchdog_irq_nce = 33;
    bp->si_stepping = dev.revision;
    bp->device_id = dev.device_id;
    bp->feature_exclusion = 0;  // MTL buttress: both tiles enabled
    bp->sku = 0x3630;
    bp->min_freq_pll_ratio = dev.min_ratio;
    bp->pn_freq_pll_ratio = dev.pn_ratio;
    bp->max_freq_pll_ratio = dev.max_ratio;
    bp->default_trace_level = LOG_INFO;
    bp->tracing_buff_message_format_mask = 1;  // string format
    bp->trace_destination_mask = TRACE_DEST_VERBOSE;
    bp->trace_hw_component_mask = ~0ull;
    bp->crit_tracing_buff_addr = g_log_crit.vpu;
    bp->crit_tracing_buff_size = uint32_t(g_log_crit.size);
    bp->verbose_tracing_buff_addr = g_log_verb.vpu;
    bp->verbose_tracing_buff_size = uint32_t(g_log_verb.size);
    bp->punit_telemetry_sram_base = rb(BTRS_TELEMETRY_OFFSET);
    bp->punit_telemetry_sram_size = rb(BTRS_TELEMETRY_SIZE);
    bp->vpu_telemetry_enable = rb(BTRS_TELEMETRY_ENABLE);
    bp->vpu_scheduling_mode = 0;  // OS scheduling: the host submits jobs itself
    bp->dvfs_mode = 0;
    bp->d0i3_delayed_entry = 1;
    flush(g_bp.cpu, g_bp.size);
    out("  boot params: magic 0x%x, IPC 0x%lx+%u, telemetry 0x%lx+%lu en %u, PLL %u/%u/%u, OS scheduling",
        bp->magic, bp->ipc_header_area_start, bp->ipc_header_area_size, bp->punit_telemetry_sram_base,
        bp->punit_telemetry_sram_size, bp->vpu_telemetry_enable, bp->min_freq_pll_ratio, bp->pn_freq_pll_ratio,
        bp->max_freq_pll_ratio);
    if (!mmu::commit()) {
        out("  MMU TLB invalidate FAILED");
        mmu::report();
        return false;
    }

    out("Start firmware:");
    // ivpu_hw_ip_snoop_disable_37xx(): page-table walks without snooping (reads).
    uint32_t v = rv(HOST_IF_TCU_PTW_OVERRIDES);
    v = (v | PTW_NOSNOOP_OVERRIDE_EN | PTW_AR_NOSNOOP) & ~PTW_AW_NOSNOOP;
    wv(HOST_IF_TCU_PTW_OVERRIDES, v);
    // ivpu_hw_ip_tbu_mmu_enable_37xx()
    wv(HOST_IF_TBU_MMUSSIDV, rv(HOST_IF_TBU_MMUSSIDV) | TBU0_AW | TBU0_AR | TBU2_AW | TBU2_AR);
    out("  TCU_PTW_OVERRIDES=0x%08x TBU_MMUSSIDV=0x%08x", rv(HOST_IF_TCU_PTW_OVERRIDES), rv(HOST_IF_TBU_MMUSSIDV));
    dump_state("before start");
    // soc_cpu_boot_37xx(): run the LEON CPU, then hand it the entry point.
    v = rv(CPU_SS_LEON_RT_VEC);
    v = (v | RT_RSTRUN0) & ~RT_RSTVEC;
    wv(CPU_SS_LEON_RT_VEC, v);
    v |= RT_RESUME0;
    wv(CPU_SS_LEON_RT_VEC, v);
    v &= ~RT_RESUME0;
    wv(CPU_SS_LEON_RT_VEC, v);
    uint32_t la = uint32_t(h.entry_point >> 9);
    wv(HOST_SS_LOADING_ADDRESS_LO, la);
    wv(HOST_SS_LOADING_ADDRESS_LO, la | 1);  // DONE
    out("  NPU CPU released, entry 0x%lx (LOADING_ADDRESS_LO=0x%08x)", h.entry_point, la | 1);

    bool ok = wait_for_ready();
    dump_state(ok ? "ready" : "no answer");
    mmu::report();
    save_log(g_log_crit, "critical", "/sys/npu/fw-critical.txt", ok ? 0 : 10);
    save_log(g_log_verb, "verbose", "/sys/npu/fw-verbose.txt", ok ? 5 : 30);
    out(ok ? "  FIRMWARE READY: the NPU answered \"BOOT\"" : "  firmware did not report ready");
    return ok;
}

}  // namespace npu::boot
