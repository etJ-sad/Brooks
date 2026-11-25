// SPDX-License-Identifier: GPL-2.0-only
// USB host side: xHCI controllers, device enumeration, mass storage.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace usb {

struct Controller;

struct Device {
    Controller* hc;
    int slot, port, speed;
    uint16_t vid, pid;
    uint8_t dev_class, dev_subclass, dev_proto;
    char product[64];
    // bulk endpoints (mass storage)
    uint8_t bulk_in, bulk_out;  // endpoint addresses (0 = none)
    uint16_t mps_in, mps_out;
    uint8_t iface;
    void* priv;  // controller-private slot data
    bool priv2_probed;  // class driver already attached
};

struct SetupPacket {
    uint8_t bmRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
};

// Scan PCI for xHCI controllers and enumerate devices on root ports.
// `log` receives human-readable progress lines. Returns number of devices.
int init(void (*log)(const char* line));
int device_count();
Device* device(int i);

// Transfers (synchronous, polled). Return bytes transferred or -1.
int control(Device* d, const SetupPacket& s, void* data);
int bulk(Device* d, bool in, void* data, uint32_t len);
bool configure_bulk(Device* d);  // set up bulk_in/bulk_out endpoints on the controller
bool clear_halt(Device* d, uint8_t ep_addr);

const char* speed_name(int speed);

}  // namespace usb

namespace msc {
// Probe all USB mass storage devices and register them as block devices.
int probe_all(void (*log)(const char* line));
}  // namespace msc
