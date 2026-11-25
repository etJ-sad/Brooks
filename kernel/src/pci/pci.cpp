// SPDX-License-Identifier: GPL-2.0-only
#include "pci/pci.h"
#include "arch/x86_64/cpu.h"

namespace pci {
namespace {
constexpr int MAX_DEVICES = 512;
Device g_devs[MAX_DEVICES];
int g_count = 0;
}  // namespace

uint32_t read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u | (uint32_t(bus) << 16) | (uint32_t(dev & 31) << 11) | (uint32_t(fn & 7) << 8) |
                    (off & 0xFC);
    cpu::outl(0xCF8, addr);
    return cpu::inl(0xCFC);
}

static void add(uint8_t bus, uint8_t dev, uint8_t fn, uint32_t id) {
    if (g_count >= MAX_DEVICES) return;
    Device& d = g_devs[g_count++];
    d.bus = bus;
    d.dev = dev;
    d.fn = fn;
    d.vendor = uint16_t(id);
    d.device = uint16_t(id >> 16);
    uint32_t cr = read32(bus, dev, fn, 0x08);
    d.rev = uint8_t(cr);
    d.progif = uint8_t(cr >> 8);
    d.subcls = uint8_t(cr >> 16);
    d.cls = uint8_t(cr >> 24);
    d.header_type = uint8_t(read32(bus, dev, fn, 0x0C) >> 16);
    if ((d.header_type & 0x7F) == 0) {
        uint32_t ss = read32(bus, dev, fn, 0x2C);
        d.subsys_vendor = uint16_t(ss);
        d.subsys_id = uint16_t(ss >> 16);
    } else {
        d.subsys_vendor = d.subsys_id = 0;
    }
}

int scan() {
    g_count = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id = read32(uint8_t(bus), uint8_t(dev), 0, 0);
            if (uint16_t(id) == 0xFFFF || uint16_t(id) == 0) continue;
            add(uint8_t(bus), uint8_t(dev), 0, id);
            bool multi = (read32(uint8_t(bus), uint8_t(dev), 0, 0x0C) >> 16) & 0x80;
            if (!multi) continue;
            for (int fn = 1; fn < 8; fn++) {
                uint32_t fid = read32(uint8_t(bus), uint8_t(dev), uint8_t(fn), 0);
                if (uint16_t(fid) == 0xFFFF || uint16_t(fid) == 0) continue;
                add(uint8_t(bus), uint8_t(dev), uint8_t(fn), fid);
            }
        }
    }
    return g_count;
}

int count() { return g_count; }
const Device& at(int i) { return g_devs[i]; }

const char* vendor_name(uint16_t v) {
    switch (v) {
    case 0x8086: return "Intel";
    case 0x8087: return "Intel";
    case 0x10DE: return "NVIDIA";
    case 0x1002: return "AMD";
    case 0x1022: return "AMD";
    case 0x10EC: return "Realtek";
    case 0x144D: return "Samsung";
    case 0x15B7: return "SanDisk/WD";
    case 0x1C5C: return "SK hynix";
    case 0x1E0F: return "KIOXIA";
    case 0x126F: return "Silicon Motion";
    case 0x1987: return "Phison";
    case 0x2646: return "Kingston";
    case 0xC0A9: return "Micron/Crucial";
    case 0x1344: return "Micron";
    case 0x14E4: return "Broadcom";
    case 0x168C: return "Qualcomm Atheros";
    case 0x17CB: return "Qualcomm";
    case 0x1B21: return "ASMedia";
    case 0x1B36: return "Red Hat (QEMU)";
    case 0x1AF4: return "Red Hat virtio";
    case 0x1234: return "QEMU";
    case 0x1217: return "O2 Micro";
    default: return "";
    }
}

const char* class_name(uint8_t c, uint8_t s, uint8_t p) {
    switch (c) {
    case 0x00: return "Unclassified";
    case 0x01:
        switch (s) {
        case 0x01: return "IDE controller";
        case 0x06: return "SATA controller";
        case 0x07: return "SAS controller";
        case 0x08: return p == 0x02 ? "NVMe SSD" : "NVM controller";
        case 0x04: return "RAID controller";
        default: return "Storage";
        }
    case 0x02:
        switch (s) {
        case 0x00: return "Ethernet";
        case 0x80: return "Network (Wi-Fi?)";
        default: return "Network";
        }
    case 0x03:
        switch (s) {
        case 0x00: return "VGA display";
        case 0x02: return "3D display";
        default: return "Display";
        }
    case 0x04:
        switch (s) {
        case 0x01: return "Audio";
        case 0x03: return "HD Audio";
        case 0x80: return "Multimedia";
        default: return "Multimedia";
        }
    case 0x05: return s == 0x00 ? "RAM controller" : "Memory controller";
    case 0x06:
        switch (s) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA/LPC bridge";
        case 0x04: return "PCI bridge / root port";
        case 0x80: return "Bridge";
        default: return "Bridge";
        }
    case 0x07: return s == 0x80 ? "Comm (MEI?)" : "Communication";
    case 0x08:
        switch (s) {
        case 0x05: return "SD host";
        case 0x80: return "System peripheral";
        default: return "System peripheral";
        }
    case 0x09: return "Input";
    case 0x0B: return "Processor";
    case 0x0C:
        switch (s) {
        case 0x03:
            switch (p) {
            case 0x30: return "USB xHCI";
            case 0x40: return "USB4";
            case 0x20: return "USB EHCI";
            case 0xFE: return "USB device";
            default: return "USB";
            }
        case 0x05: return "SMBus";
        case 0x07: return "IPMI";
        case 0x80: return "Serial bus (I2C/SPI)";
        default: return "Serial bus";
        }
    case 0x0D: return "Wireless";
    case 0x10: return "Encryption";
    case 0x11: return s == 0x80 ? "Signal processing" : "Signal processing";
    case 0x12: return "Processing accelerator";
    case 0x13: return "Non-essential";
    case 0xFF: return "Vendor specific";
    default: return "Unknown";
    }
}

const char* intel_npu_name(const Device& d) {
    if (d.vendor != 0x8086) return nullptr;
    switch (d.device) {  // IDs as used by the Linux ivpu driver
    case 0x7D1D: return "Meteor Lake NPU (NPU37xx)";
    case 0xAD1D: return "Arrow Lake NPU (NPU37xx)";
    case 0x643E: return "Lunar Lake NPU (NPU40xx)";
    case 0xB03E: return "Panther Lake NPU (NPU50xx)";
    case 0xFD3E: return "Wildcat Lake NPU (NPU50xx)";
    default: break;
    }
    if (d.cls == 0x12 && d.subcls == 0x00) return "Intel NPU (unknown generation)";
    return nullptr;
}

}  // namespace pci
