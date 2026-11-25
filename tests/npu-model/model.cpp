// SPDX-License-Identifier: GPL-2.0-only
// Host register model of the NPU 37xx + MTL buttress, enough to run step A and
// step B of npu::bringup(): power handshakes, NPU MMU (CR0/IRQ acks, command
// queue), and a fake firmware that walks the NPU page tables exactly like the
// MMU would, checks the boot params and the copied image, writes a log line and
// posts the "BOOT" IPC message.
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>
#include "fs/vfs.h"
#include "mm/pmm.h"
#include "npu/npu.h"

uint8_t* g_bar0;
volatile uint32_t g_b[0x400];
size_t mm::g_live = 0;
fs::Node fs::g_fw;
const char* fs::g_fw_path = nullptr;
std::atomic<bool> stop{false};
int g_mode = 0;  // 0 = good firmware, 1 = firmware never answers, 2 = no firmware file
const char* g_fw_file = nullptr;
std::vector<const char*> g_errors;

#define B(o) g_b[(o) / 4]
static volatile uint32_t& V(uint32_t o) { return *reinterpret_cast<volatile uint32_t*>(g_bar0 + o); }
static volatile uint64_t& V64(uint32_t o) { return *reinterpret_cast<volatile uint64_t*>(g_bar0 + o); }

static void err(const char* e) {
    g_errors.push_back(e);
    printf("MODEL ERROR: %s\n", e);
}

// NPU MMU walk: STRTAB -> STE[0] -> CD[0] -> 4-level tables.
static uint8_t* npu_ptr(uint64_t va) {
    uint64_t strtab = V64(0x200080) & (((1ull << 52) - 1) & ~63ull);
    if (!(V(0x200020) & 1)) { err("access with MMU disabled"); return nullptr; }
    uint64_t* ste = (uint64_t*)strtab;
    if (!(ste[0] & 1) || ((ste[0] >> 1) & 7) != 5) { err("STE0 not valid/stage1"); return nullptr; }
    uint64_t* cd = (uint64_t*)(ste[0] & (((1ull << 52) - 1) & ~63ull));
    if (!(cd[0] >> 31 & 1)) { err("CD0 not valid"); return nullptr; }
    if ((cd[0] & 63) != 16) { err("CD0 T0SZ"); return nullptr; }
    uint64_t t = cd[1] & (((1ull << 52) - 1) & ~15ull);
    for (int shift = 39; shift >= 12; shift -= 9) {
        uint64_t e = ((uint64_t*)t)[(va >> shift) & 511];
        if ((e & 3) != 3) { printf("  model: translation fault at 0x%lx level %d\n", va, shift); return nullptr; }
        if (shift == 12 && (e & 0xC40) != 0xC40) { err("PTE without AF/NG/USER"); return nullptr; }
        t = e & (((1ull << 48) - 1) & ~0xFFFull);
    }
    return (uint8_t*)(t | (va & 0xFFF));
}

static void firmware_boot() {
    const uint8_t* f = fs::g_fw.data;
    uint64_t bp_addr = *(const uint64_t*)(f + 76), entry = *(const uint64_t*)(f + 20), load = *(const uint64_t*)(f + 8);
    uint32_t isz = *(const uint32_t*)(f + 16);
    uint32_t la = V(0x41040);
    if (la != (uint32_t(entry >> 9) | 1)) err("LOADING_ADDRESS_LO wrong");
    if ((V(0x360004) & 0x33) != 0x33) err("TBU MMUSSIDV not enabled");
    if ((V(0x360000) & 0x38) != 0x28) err("TCU PTW overrides wrong");
    uint8_t* bp = npu_ptr(bp_addr);
    if (!bp) { err("boot params unmapped"); return; }
    if (*(uint32_t*)bp != 0x10000) err("boot params magic");
    for (uint64_t off = 0; off < isz; off += (off < 65536 ? 1 : 4093)) {
        uint8_t* p = npu_ptr(load + off);
        if (!p || *p != f[8192 + off]) { err("image byte mismatch through NPU MMU"); break; }
    }
    if (g_mode == 1) return;
    uint64_t hdr_area = *(uint64_t*)(bp + 0x100);
    // verbose_tracing_buff_addr: 0x300 + host_version_id..vpu_telemetry_enable(4+4+8+8+8+4*4+8+8+4) + crit addr/size (8+4)
    uint64_t verb = *(uint64_t*)(bp + 0x300 + 4 + 4 + 8 + 8 + 8 + 16 + 8 + 8 + 4 + 8 + 4);
    uint8_t* lg = npu_ptr(verb);
    if (lg) {
        const char* msg = "model firmware: boot params ok\nmodel firmware: posting BOOT\n";
        uint32_t* w = (uint32_t*)lg;
        w[16] = 0xCAFECAFE; w[20] = 1 << 20; *(uint16_t*)(lg + 86) = 128;
        memcpy(lg + 128, msg, strlen(msg));
        w[17] = 128 + strlen(msg);
    } else err("verbose log unmapped");
    uint8_t* h = npu_ptr(hdr_area + 128);
    if (!h) { err("IPC header area unmapped"); return; }
    *(uint32_t*)h = 0x424f4f54; *(uint32_t*)(h + 4) = 0; *(uint16_t*)(h + 8) = 0x3ff; h[12] = 1;
    asm volatile("mfence" ::: "memory");
    V(0x200f4) = uint32_t(hdr_area + 128);
    V(0x200fc) = 1u << 16;
    while (!stop && *(volatile uint8_t*)(h + 12) != 0) std::this_thread::yield();
    V(0x200fc) = 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <vpu_37xx_v1.bin> <mode: 0 ready, 1 silent firmware, 2 no firmware>\n", argv[0]);
        return 2;
    }
    g_fw_file = argv[1];
    g_mode = atoi(argv[2]);
    g_bar0 = (uint8_t*)calloc(1, 0x06030000);  // BAR0 up to the CPU_SS timer page
    if (g_mode != 2) {
        FILE* fp = fopen(g_fw_file, "rb");
        if (!fp) return 2;
        fseek(fp, 0, SEEK_END); size_t n = ftell(fp); fseek(fp, 0, SEEK_SET);
        fs::g_fw.data = (uint8_t*)malloc(n); fs::g_fw.size = n; fs::g_fw.dir = false;
        if (fread(fs::g_fw.data, 1, n, fp) != n) return 2;
        fclose(fp);
        fs::g_fw_path = "x";
    }
    B(0x24) = 0x0a05; B(0x28) = 0x1e;
    V(0x200000) = 0x080f3e0f; V(0x200004) = 0x0e739d18; V(0x20000c) = 0x3c; V(0x200014) = 0x00040070;
    std::thread hw([] {
        bool booted = false;
        while (!stop) {
            if (B(0x14) & 1) {
                bool on = (B(0x0c) & 0xffff) != 0;
                B(0x40) = on ? 2 : 0; B(0x44) = on ? 1 : 0; B(0x14) &= ~1u;
            }
            if (B(0x50) & 1) { B(0x50) &= ~1u; V(0x200020) = 0; V(0x200024) = 0; booted = false; V(0x41040) = 0; }
            uint32_t island = V(0x30024) & 8; V(0x3002c) = island;
            V(0x158) = V(0x154) & 1;
            V(0x164) = V(0x160) & 3;
            if (island) V64(0x06029000) = V64(0x06029000) + 1;
            V(0x200024) = V(0x200020);
            V(0x200054) = V(0x200050);
            uint32_t prod = V(0x200098);
            if ((V(0x200020) & 8) && (V(0x20009c) & 0xffffff) != prod) {
                uint64_t base = V64(0x200090) & (((1ull << 52) - 1) & ~31ull);
                for (uint32_t c = V(0x20009c) & 0x1f; c != (prod & 0x1f); c = (c + 1) & 0x1f) {
                    uint8_t op = ((uint64_t*)base)[(c & 15) * 2] & 0xff;
                    if (op != 0x4 && op != 0x30 && op != 0x46) err("bad MMU command");
                }
                V(0x20009c) = prod;
            }
            if (!booted && (V(0x41040) & 1)) { booted = true; firmware_boot(); }
        }
    });
    npu::Result r = npu::bringup([](const char* l) { printf("%s\n", l); }, true);
    stop = true; hw.join();
    printf("result: up %d down %d fw_tried %d fw_ready %d, model errors %zu, pages still allocated %zu\n", r.power_up,
           r.power_down, r.fw_tried, r.fw_ready, g_errors.size(), mm::g_live);
    bool want_ready = g_mode == 0;
    return (r.power_up && r.power_down && r.fw_ready == want_ready && g_errors.empty() && mm::g_live == 0) ? 0 : 1;
}
