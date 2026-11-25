// SPDX-License-Identifier: GPL-2.0-only
// xHCI host controller driver (polled, no interrupts). Enough for root-port
// devices: control transfers and bulk endpoints. No hubs yet.
#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "pci/pci.h"
#include "usb/usb.h"

namespace usb {

namespace {

struct Trb {
    uint32_t d[4];
};

enum TrbType : uint32_t {
    TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6,
    TRB_ENABLE_SLOT = 9, TRB_DISABLE_SLOT = 10, TRB_ADDRESS_DEVICE = 11, TRB_CONFIGURE_EP = 12,
    TRB_EVALUATE_CTX = 13, TRB_RESET_EP = 14, TRB_SET_TR_DEQ = 16,
    TRB_EV_TRANSFER = 32, TRB_EV_CMD_COMPLETE = 33, TRB_EV_PORT_STATUS = 34,
};

constexpr int RING_TRBS = 256;  // one 4 KiB page

struct Ring {
    Trb* v;
    uint64_t phys;
    int idx;
    uint32_t cycle;
};

bool ring_init(Ring& r) {
    r.phys = mm::alloc_pages(1);
    if (!r.phys) return false;
    r.v = mm::phys_to_virt<Trb>(r.phys);
    r.idx = 0;
    r.cycle = 1;
    // Link TRB back to the start, toggle cycle.
    Trb& l = r.v[RING_TRBS - 1];
    l.d[0] = uint32_t(r.phys);
    l.d[1] = uint32_t(r.phys >> 32);
    l.d[2] = 0;
    l.d[3] = (TRB_LINK << 10) | (1 << 1);
    return true;
}

// Writes a TRB (cycle bit filled in) and returns its physical address.
uint64_t ring_push(Ring& r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
    Trb& t = r.v[r.idx];
    t.d[0] = d0;
    t.d[1] = d1;
    t.d[2] = d2;
    asm volatile("" ::: "memory");
    t.d[3] = (d3 & ~1u) | r.cycle;
    uint64_t at = r.phys + uint64_t(r.idx) * sizeof(Trb);
    if (++r.idx == RING_TRBS - 1) {
        Trb& l = r.v[RING_TRBS - 1];
        l.d[3] = (l.d[3] & ~1u) | r.cycle;  // hand the link TRB to the controller
        r.idx = 0;
        r.cycle ^= 1;
    }
    return at;
}

}  // namespace

struct Controller {
    pci::Device pci;
    volatile uint8_t* cap;
    volatile uint32_t* op;
    volatile uint32_t* rt;
    volatile uint32_t* db;
    int max_slots, max_ports, ctx_size;
    uint64_t* dcbaa;
    Ring cmd;
    Trb* evt;
    uint64_t evt_phys;
    int evt_idx;
    uint32_t evt_cycle;
    uint8_t port_usb3[256];  // 1 if root port speaks USB3 (from Supported Protocol caps)
};

namespace {

struct Ep {
    Ring ring;
    bool ok;
};

struct Slot {
    uint64_t in_phys, out_phys;
    uint8_t* in;   // input context (virt)
    uint8_t* out;  // device context (virt)
    Ep ep[32];     // by DCI
};

Controller* g_hcs[8];
int g_nhcs = 0;
Device* g_devs[32];
int g_ndevs = 0;
void (*g_log)(const char*) = nullptr;

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* fmt, ...) {
    if (!g_log) return;
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log(buf);
}

void delay_ms(uint64_t ms) {
    uint64_t end = apic::uptime_ms() + ms;
    while (apic::uptime_ms() < end) cpu::pause();
}

template <typename F>
bool wait_until(F cond, uint64_t ms) {
    uint64_t end = apic::uptime_ms() + ms;
    while (!cond()) {
        if (apic::uptime_ms() > end) return false;
        cpu::pause();
    }
    return true;
}

inline uint32_t op_r(Controller* hc, uint32_t off) { return hc->op[off / 4]; }
inline void op_w(Controller* hc, uint32_t off, uint32_t v) { hc->op[off / 4] = v; }
inline volatile uint32_t* portsc(Controller* hc, int port) { return &hc->op[(0x400 + 0x10 * (port - 1)) / 4]; }
inline volatile uint32_t* ir0(Controller* hc) { return &hc->rt[0x20 / 4]; }  // interrupter 0 register set

void ring_doorbell(Controller* hc, int slot, uint32_t target) {
    asm volatile("mfence" ::: "memory");
    hc->db[slot] = target;
}

// Polls the event ring for the next event. Returns false on timeout.
bool next_event(Controller* hc, Trb& out, uint64_t timeout_ms) {
    uint64_t end = apic::uptime_ms() + timeout_ms;
    for (;;) {
        Trb& e = hc->evt[hc->evt_idx];
        if ((e.d[3] & 1) == hc->evt_cycle) {
            out = e;
            if (++hc->evt_idx == RING_TRBS) {
                hc->evt_idx = 0;
                hc->evt_cycle ^= 1;
            }
            uint64_t erdp = hc->evt_phys + uint64_t(hc->evt_idx) * sizeof(Trb);
            volatile uint32_t* ir = ir0(hc);
            ir[6] = uint32_t(erdp) | (1 << 3);  // ERDP low, clear EHB
            ir[7] = uint32_t(erdp >> 32);
            return true;
        }
        if (apic::uptime_ms() > end) return false;
        cpu::pause();
    }
}

// Waits for a specific event type whose TRB pointer matches `trb` (0 = any).
bool wait_event(Controller* hc, uint32_t type, uint64_t trb, Trb& out, uint64_t timeout_ms = 2000) {
    uint64_t end = apic::uptime_ms() + timeout_ms;
    for (;;) {
        uint64_t now = apic::uptime_ms();
        if (now > end) return false;
        Trb e;
        if (!next_event(hc, e, end - now)) return false;
        uint32_t t = (e.d[3] >> 10) & 0x3F;
        uint64_t ptr = e.d[0] | (uint64_t(e.d[1]) << 32);
        if (t == type && (trb == 0 || ptr == trb)) {
            out = e;
            return true;
        }
        // other events (port status change, stale transfers) are dropped
    }
}

// Issues a command; returns completion code (1 = success), slot id in *slot_out.
int command(Controller* hc, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, int* slot_out = nullptr) {
    uint64_t at = ring_push(hc->cmd, d0, d1, d2, d3);
    ring_doorbell(hc, 0, 0);
    Trb e;
    if (!wait_event(hc, TRB_EV_CMD_COMPLETE, at, e, 3000)) return -1;
    if (slot_out) *slot_out = int(e.d[3] >> 24);
    return int(e.d[2] >> 24);
}

void bios_handoff(Controller* hc) {
    uint32_t hcc1 = *reinterpret_cast<volatile uint32_t*>(hc->cap + 0x10);
    uint32_t off = (hcc1 >> 16) * 4;
    while (off) {
        volatile uint32_t* c = reinterpret_cast<volatile uint32_t*>(hc->cap + off);
        uint32_t v = c[0];
        uint8_t id = uint8_t(v);
        if (id == 1) {  // USB Legacy Support
            c[0] = v | (1 << 24);  // OS owned semaphore
            bool ok = wait_until([&] { return !(c[0] & (1 << 16)); }, 1000);
            c[1] = c[1] & 0x1F1EE;  // disable SMIs, clear status bits we don't own
            logf("  BIOS handoff %s", ok ? "ok" : "timeout (forcing)");
            if (!ok) c[0] = (c[0] & ~(1u << 16)) | (1 << 24);
        } else if (id == 2) {  // Supported Protocol
            uint8_t major = uint8_t(v >> 24);
            uint32_t ports = c[2];
            int first = ports & 0xFF, count = (ports >> 8) & 0xFF;
            for (int p = first; p < first + count && p < 256; p++) hc->port_usb3[p] = major >= 3;
        }
        uint32_t next = (v >> 8) & 0xFF;
        off = next ? off + next * 4 : 0;
    }
}

bool hc_init(Controller* hc) {
    uint64_t bar = pci::bar_address(hc->pci, 0);
    if (!bar) return false;
    pci::enable_mmio_dma(hc->pci);
    hc->cap = static_cast<volatile uint8_t*>(vmm::map(bar, 0x10000, vmm::Uncached));
    if (!hc->cap) return false;
    uint8_t caplen = hc->cap[0];
    uint32_t hcs1 = *reinterpret_cast<volatile uint32_t*>(hc->cap + 0x04);
    uint32_t hcs2 = *reinterpret_cast<volatile uint32_t*>(hc->cap + 0x08);
    uint32_t hcc1 = *reinterpret_cast<volatile uint32_t*>(hc->cap + 0x10);
    uint32_t dboff = *reinterpret_cast<volatile uint32_t*>(hc->cap + 0x14) & ~3u;
    uint32_t rtsoff = *reinterpret_cast<volatile uint32_t*>(hc->cap + 0x18) & ~0x1Fu;
    hc->op = reinterpret_cast<volatile uint32_t*>(hc->cap + caplen);
    hc->rt = reinterpret_cast<volatile uint32_t*>(hc->cap + rtsoff);
    hc->db = reinterpret_cast<volatile uint32_t*>(hc->cap + dboff);
    hc->max_slots = int(hcs1 & 0xFF);
    hc->max_ports = int(hcs1 >> 24);
    hc->ctx_size = (hcc1 & (1 << 2)) ? 64 : 32;
    if (rtsoff + 0x40 > 0x10000 || dboff + 4 * 256 > 0x10000) {
        // registers beyond the first 64 KiB: remap bigger
        hc->cap = static_cast<volatile uint8_t*>(vmm::map(bar, 0x20000, vmm::Uncached));
        hc->op = reinterpret_cast<volatile uint32_t*>(hc->cap + caplen);
        hc->rt = reinterpret_cast<volatile uint32_t*>(hc->cap + rtsoff);
        hc->db = reinterpret_cast<volatile uint32_t*>(hc->cap + dboff);
    }
    logf("xHCI %02x:%02x.%x %04x:%04x at 0x%lx: %d ports, %d slots, ctx %d B, AC64 %d", hc->pci.bus, hc->pci.dev,
         hc->pci.fn, hc->pci.vendor, hc->pci.device, bar, hc->max_ports, hc->max_slots, hc->ctx_size, int(hcc1 & 1));

    bios_handoff(hc);

    // Stop and reset.
    op_w(hc, 0x00, op_r(hc, 0x00) & ~1u);
    if (!wait_until([&] { return op_r(hc, 0x04) & 1; }, 100)) logf("  warning: controller did not halt");
    op_w(hc, 0x00, 1 << 1);  // HCRST
    delay_ms(10);
    if (!wait_until([&] { return !(op_r(hc, 0x00) & 2) && !(op_r(hc, 0x04) & (1 << 11)); }, 1000)) {
        logf("  reset timeout");
        return false;
    }

    op_w(hc, 0x38, uint32_t(hc->max_slots));  // CONFIG.MaxSlotsEn

    uint64_t dcbaa_phys = mm::alloc_pages(1);
    if (!dcbaa_phys) return false;
    hc->dcbaa = mm::phys_to_virt<uint64_t>(dcbaa_phys);

    // Scratchpad buffers.
    int nscratch = int(((hcs2 >> 21) & 0x1F) << 5 | ((hcs2 >> 27) & 0x1F));
    if (nscratch) {
        uint64_t arr = mm::alloc_pages(1);
        if (!arr) return false;
        auto* a = mm::phys_to_virt<uint64_t>(arr);
        uint32_t pagesize = (op_r(hc, 0x08) & 0xFFFF) << 12;
        size_t pages = pagesize / mm::PAGE;
        for (int i = 0; i < nscratch && i < 512; i++) a[i] = mm::alloc_pages(pages ? pages : 1);
        hc->dcbaa[0] = arr;
    }
    op_w(hc, 0x30, uint32_t(dcbaa_phys));
    op_w(hc, 0x34, uint32_t(dcbaa_phys >> 32));

    // Command ring.
    if (!ring_init(hc->cmd)) return false;
    op_w(hc, 0x18, uint32_t(hc->cmd.phys) | 1);
    op_w(hc, 0x1C, uint32_t(hc->cmd.phys >> 32));

    // Event ring with one segment.
    hc->evt_phys = mm::alloc_pages(1);
    uint64_t erst_phys = mm::alloc_pages(1);
    if (!hc->evt_phys || !erst_phys) return false;
    hc->evt = mm::phys_to_virt<Trb>(hc->evt_phys);
    hc->evt_idx = 0;
    hc->evt_cycle = 1;
    auto* erst = mm::phys_to_virt<uint64_t>(erst_phys);
    erst[0] = hc->evt_phys;
    erst[1] = RING_TRBS;
    volatile uint32_t* ir = ir0(hc);
    ir[0] = ir[0] & ~2u;  // IMAN: interrupts off (polled)
    ir[2] = 1;            // ERSTSZ
    ir[6] = uint32_t(hc->evt_phys);
    ir[7] = uint32_t(hc->evt_phys >> 32);
    ir[4] = uint32_t(erst_phys);
    ir[5] = uint32_t(erst_phys >> 32);

    // Run.
    op_w(hc, 0x00, op_r(hc, 0x00) | 1);
    if (!wait_until([&] { return !(op_r(hc, 0x04) & 1); }, 100)) {
        logf("  controller did not start");
        return false;
    }
    // Power all ports (needed when PPC = 1).
    for (int p = 1; p <= hc->max_ports; p++) {
        uint32_t v = *portsc(hc, p);
        if (!(v & (1 << 9))) *portsc(hc, p) = (v & 0x0E00C3E0) | (1 << 9);
    }
    return true;
}

constexpr uint32_t PORT_RO = 0x01 | 0x08 | 0x3C00 | 0x01000000 | 0x40000000;
constexpr uint32_t PORT_RWS = 0x1E0 | 0x200 | 0xC000 | 0x0E000000;
constexpr uint32_t PORT_CHANGE = 0x00FE0000;

inline uint32_t port_neutral(uint32_t v) { return v & (PORT_RO | PORT_RWS); }

// Returns speed (1..5) once the port is enabled, 0 if nothing usable.
int port_reset(Controller* hc, int port) {
    volatile uint32_t* ps = portsc(hc, port);
    uint32_t v = *ps;
    if (!(v & 1)) return 0;  // no device
    if (!(v & 2)) {          // not enabled: USB2 needs a reset (USB3 links up by itself)
        *ps = port_neutral(v) | (1 << 4);  // PR
        if (!wait_until([&] { return (*ps & (1 << 21)) != 0; }, 500)) return 0;  // PRC
        delay_ms(20);
    }
    v = *ps;
    *ps = port_neutral(v) | PORT_CHANGE;  // clear all change bits
    if (!(v & 2)) return 0;
    return int((v >> 10) & 0xF);
}

inline uint32_t* ctx(uint8_t* base, int ctx_size, int index) {
    return reinterpret_cast<uint32_t*>(base + index * ctx_size);
}

uint16_t default_mps0(int speed) {
    switch (speed) {
    case 2: return 8;     // low speed
    case 1: return 64;    // full speed (8..64; 64 works for storage devices)
    case 3: return 64;    // high speed
    default: return 512;  // super speed and up
    }
}

Slot* slot_of(Device* d) { return static_cast<Slot*>(d->priv); }

int control_raw(Controller* hc, int slot, Slot* s, const SetupPacket& sp, void* data) {
    Ring& r = s->ep[1].ring;
    bool in = sp.bmRequestType & 0x80;
    uint64_t buf_phys = 0;
    uint8_t* buf = nullptr;
    if (sp.wLength) {
        buf_phys = mm::alloc_pages(1);
        if (!buf_phys || sp.wLength > mm::PAGE) return -1;
        buf = mm::phys_to_virt<uint8_t>(buf_phys);
        if (!in) memcpy(buf, data, sp.wLength);
    }
    uint32_t trt = sp.wLength ? (in ? 3 : 2) : 0;
    uint32_t w0 = uint32_t(sp.bmRequestType) | (uint32_t(sp.bRequest) << 8) | (uint32_t(sp.wValue) << 16);
    uint32_t w1 = uint32_t(sp.wIndex) | (uint32_t(sp.wLength) << 16);
    ring_push(r, w0, w1, 8, (TRB_SETUP << 10) | (1 << 6) | (trt << 16));
    if (sp.wLength) ring_push(r, uint32_t(buf_phys), uint32_t(buf_phys >> 32), sp.wLength, (TRB_DATA << 10) | (in ? 1u << 16 : 0));
    uint64_t st = ring_push(r, 0, 0, 0, (TRB_STATUS << 10) | (1 << 5) | ((sp.wLength && in) ? 0 : (1u << 16)));
    ring_doorbell(hc, slot, 1);
    Trb e;
    int result = -1;
    if (wait_event(hc, TRB_EV_TRANSFER, st, e, 2000)) {
        int cc = int(e.d[2] >> 24);
        if (cc == 1 || cc == 13) {
            result = sp.wLength;
            if (in && buf) memcpy(data, buf, sp.wLength);
        }
    }
    if (buf_phys) mm::free_pages(buf_phys, 1);
    return result;
}

void read_string(Device* d, uint8_t index, char* out, size_t cap) {
    out[0] = 0;
    if (!index) return;
    uint8_t buf[256];
    SetupPacket sp{0x80, 6, uint16_t(0x0300 | index), 0x0409, 255};
    int n = control(d, sp, buf);
    if (n < 2) return;
    int len = buf[0] < n ? buf[0] : n;
    size_t o = 0;
    for (int i = 2; i + 1 < len && o + 3 < cap; i += 2) {
        uint16_t c = uint16_t(buf[i] | (buf[i + 1] << 8));
        if (c < 0x80) out[o++] = char(c);
        else if (c < 0x800) { out[o++] = char(0xC0 | (c >> 6)); out[o++] = char(0x80 | (c & 0x3F)); }
        else { out[o++] = '?'; }
    }
    out[o] = 0;
    while (o > 0 && out[o - 1] == ' ') out[--o] = 0;
}

void enumerate_port(Controller* hc, int port) {
    int speed = port_reset(hc, port);
    if (!speed) return;
    int slot = 0;
    int cc = command(hc, 0, 0, 0, TRB_ENABLE_SLOT << 10, &slot);
    if (cc != 1 || slot <= 0) {
        logf("  port %d: Enable Slot failed (cc %d)", port, cc);
        return;
    }
    auto* s = static_cast<Slot*>(kzalloc(sizeof(Slot)));
    s->in_phys = mm::alloc_pages(1);
    s->out_phys = mm::alloc_pages(1);
    s->in = mm::phys_to_virt<uint8_t>(s->in_phys);
    s->out = mm::phys_to_virt<uint8_t>(s->out_phys);
    hc->dcbaa[slot] = s->out_phys;
    ring_init(s->ep[1].ring);
    s->ep[1].ok = true;

    const int cs = hc->ctx_size;
    uint32_t* icc = ctx(s->in, cs, 0);
    icc[1] = 0x3;  // add slot + EP0
    uint32_t* sc = ctx(s->in, cs, 1);
    sc[0] = (uint32_t(speed) << 20) | (1u << 27);
    sc[1] = uint32_t(port) << 16;
    uint32_t* ep0 = ctx(s->in, cs, 2);
    ep0[1] = (3u << 1) | (4u << 3) | (uint32_t(default_mps0(speed)) << 16);
    ep0[2] = uint32_t(s->ep[1].ring.phys) | 1;
    ep0[3] = uint32_t(s->ep[1].ring.phys >> 32);
    ep0[4] = 8;
    cc = command(hc, uint32_t(s->in_phys), uint32_t(s->in_phys >> 32), 0,
                 (TRB_ADDRESS_DEVICE << 10) | (uint32_t(slot) << 24));
    if (cc != 1) {
        logf("  port %d: Address Device failed (cc %d)", port, cc);
        return;
    }
    delay_ms(10);

    auto* d = static_cast<Device*>(kzalloc(sizeof(Device)));
    d->hc = hc;
    d->slot = slot;
    d->port = port;
    d->speed = speed;
    d->priv = s;

    uint8_t dd[18];
    SetupPacket get_dev{0x80, 6, 0x0100, 0, 18};
    if (control(d, get_dev, dd) < 18) {
        logf("  port %d: GET_DESCRIPTOR(device) failed", port);
        return;
    }
    d->vid = uint16_t(dd[8] | (dd[9] << 8));
    d->pid = uint16_t(dd[10] | (dd[11] << 8));
    d->dev_class = dd[4];
    d->dev_subclass = dd[5];
    d->dev_proto = dd[6];
    read_string(d, dd[15], d->product, sizeof(d->product));

    // Configuration descriptor: find a mass storage Bulk-Only interface.
    uint8_t cfg[1024];
    SetupPacket get_cfg9{0x80, 6, 0x0200, 0, 9};
    if (control(d, get_cfg9, cfg) < 9) return;
    uint16_t total = uint16_t(cfg[2] | (cfg[3] << 8));
    if (total > sizeof(cfg)) total = sizeof(cfg);
    SetupPacket get_cfg{0x80, 6, 0x0200, 0, total};
    if (control(d, get_cfg, cfg) < total) return;
    uint8_t config_value = cfg[5];
    bool in_msc = false;
    uint8_t iface_class = 0;
    for (int off = 0; off + 2 <= total && cfg[off] >= 2; off += cfg[off]) {
        uint8_t len = cfg[off], type = cfg[off + 1];
        if (type == 4 && len >= 9) {  // interface
            if (in_msc && d->bulk_in && d->bulk_out) break;
            uint8_t alt = cfg[off + 3];
            iface_class = cfg[off + 5];
            in_msc = alt == 0 && cfg[off + 5] == 8 && cfg[off + 6] == 6 && cfg[off + 7] == 0x50;
            if (in_msc) d->iface = cfg[off + 2];
            if (!d->dev_class) { d->dev_class = cfg[off + 5]; d->dev_subclass = cfg[off + 6]; d->dev_proto = cfg[off + 7]; }
        } else if (type == 5 && len >= 7 && in_msc) {  // endpoint
            uint8_t addr = cfg[off + 2], attr = cfg[off + 3];
            uint16_t mps = uint16_t(cfg[off + 4] | (cfg[off + 5] << 8)) & 0x7FF;
            if ((attr & 3) == 2) {
                if (addr & 0x80) { d->bulk_in = addr; d->mps_in = mps; }
                else { d->bulk_out = addr; d->mps_out = mps; }
            }
        }
    }
    (void)iface_class;
    SetupPacket set_cfg{0x00, 9, config_value, 0, 0};
    control(d, set_cfg, nullptr);

    logf("  port %d %s: %04x:%04x class %02x/%02x/%02x \"%s\"%s", port, speed_name(speed), d->vid, d->pid,
         d->dev_class, d->dev_subclass, d->dev_proto, d->product,
         (d->bulk_in && d->bulk_out) ? "  [mass storage BOT]" : (d->dev_class == 9 ? "  [hub: not supported yet]" : ""));
    if (g_ndevs < 32) g_devs[g_ndevs++] = d;
}

}  // namespace

const char* speed_name(int s) {
    switch (s) {
    case 1: return "FS 12M";
    case 2: return "LS 1.5M";
    case 3: return "HS 480M";
    case 4: return "SS 5G";
    case 5: return "SS+ 10G";
    default: return "?";
    }
}

int control(Device* d, const SetupPacket& s, void* data) { return control_raw(d->hc, d->slot, slot_of(d), s, data); }

bool configure_bulk(Device* d) {
    Controller* hc = d->hc;
    Slot* s = slot_of(d);
    const int cs = hc->ctx_size;
    memset(s->in, 0, mm::PAGE);
    int dci_in = (d->bulk_in & 0xF) * 2 + 1;
    int dci_out = (d->bulk_out & 0xF) * 2;
    int max_dci = dci_in > dci_out ? dci_in : dci_out;
    uint32_t* icc = ctx(s->in, cs, 0);
    icc[1] = 1u | (1u << dci_in) | (1u << dci_out);
    // slot context copied from the output context, with updated entry count
    memcpy(ctx(s->in, cs, 1), ctx(s->out, cs, 0), size_t(cs));
    uint32_t* sc = ctx(s->in, cs, 1);
    sc[0] = (sc[0] & ~(0x1Fu << 27)) | (uint32_t(max_dci) << 27);
    sc[3] = 0;
    for (int k = 0; k < 2; k++) {
        int dci = k ? dci_in : dci_out;
        uint16_t mps = k ? d->mps_in : d->mps_out;
        if (!mps) mps = d->speed >= 4 ? 1024 : 512;
        Ep& ep = s->ep[dci];
        if (!ring_init(ep.ring)) return false;
        ep.ok = true;
        uint32_t* ec = ctx(s->in, cs, dci + 1);
        ec[1] = (3u << 1) | ((k ? 6u : 2u) << 3) | (uint32_t(mps) << 16);
        ec[2] = uint32_t(ep.ring.phys) | 1;
        ec[3] = uint32_t(ep.ring.phys >> 32);
        ec[4] = 3072;  // average TRB length
    }
    int cc = command(hc, uint32_t(s->in_phys), uint32_t(s->in_phys >> 32), 0,
                     (TRB_CONFIGURE_EP << 10) | (uint32_t(d->slot) << 24));
    if (cc != 1) logf("  Configure Endpoint failed (cc %d)", cc);
    return cc == 1;
}

int bulk(Device* d, bool in, void* phys_buf_virt, uint32_t len) {
    // `phys_buf_virt` must be HHDM memory from mm::alloc_pages that does not
    // cross a 64 KiB boundary.
    Controller* hc = d->hc;
    Slot* s = slot_of(d);
    int dci = in ? (d->bulk_in & 0xF) * 2 + 1 : (d->bulk_out & 0xF) * 2;
    Ep& ep = s->ep[dci];
    if (!ep.ok) return -1;
    uint64_t phys = mm::virt_to_phys(phys_buf_virt);
    uint64_t at = ring_push(ep.ring, uint32_t(phys), uint32_t(phys >> 32), len & 0x1FFFF,
                            (TRB_NORMAL << 10) | (1 << 5) | (1 << 2));
    ring_doorbell(hc, d->slot, uint32_t(dci));
    Trb e;
    if (!wait_event(hc, TRB_EV_TRANSFER, at, e, 5000)) return -1;
    int cc = int(e.d[2] >> 24);
    uint32_t residual = e.d[2] & 0xFFFFFF;
    if (cc == 1 || cc == 13) return int(len - residual);
    if (cc == 6) return -2;  // stall
    return -1;
}

bool clear_halt(Device* d, uint8_t ep_addr) {
    Controller* hc = d->hc;
    Slot* s = slot_of(d);
    int dci = (ep_addr & 0xF) * 2 + ((ep_addr & 0x80) ? 1 : 0);
    // Reset Endpoint, then move the dequeue pointer past the failed TRB.
    command(hc, 0, 0, 0, (TRB_RESET_EP << 10) | (uint32_t(dci) << 16) | (uint32_t(d->slot) << 24));
    Ring& r = s->ep[dci].ring;
    uint64_t deq = r.phys + uint64_t(r.idx) * sizeof(Trb);
    command(hc, uint32_t(deq) | r.cycle, uint32_t(deq >> 32), 0,
            (TRB_SET_TR_DEQ << 10) | (uint32_t(dci) << 16) | (uint32_t(d->slot) << 24));
    SetupPacket sp{0x02, 1, 0, ep_addr, 0};  // CLEAR_FEATURE(ENDPOINT_HALT)
    return control(d, sp, nullptr) >= 0;
}

int init(void (*log)(const char*)) {
    g_log = log;
    if (g_nhcs) return g_ndevs;  // already initialised
    for (int i = 0; i < pci::count() && g_nhcs < 8; i++) {
        const pci::Device& p = pci::at(i);
        if (p.cls != 0x0C || p.subcls != 0x03 || p.progif != 0x30) continue;
        auto* hc = static_cast<Controller*>(kzalloc(sizeof(Controller)));
        hc->pci = p;
        if (!hc_init(hc)) {
            logf("  controller init failed");
            continue;
        }
        g_hcs[g_nhcs++] = hc;
    }
    if (!g_nhcs) logf("no xHCI controller found");
    // Give USB3 links and USB2 attach debounce time to settle.
    delay_ms(300);
    for (int h = 0; h < g_nhcs; h++) {
        Controller* hc = g_hcs[h];
        int connected = 0;
        for (int p = 1; p <= hc->max_ports; p++)
            if (*portsc(hc, p) & 1) connected++;
        logf("xHCI #%d: %d port(s) with a device", h, connected);
        for (int p = 1; p <= hc->max_ports; p++) enumerate_port(hc, p);
    }
    return g_ndevs;
}

int device_count() { return g_ndevs; }
Device* device(int i) { return (i >= 0 && i < g_ndevs) ? g_devs[i] : nullptr; }

}  // namespace usb
