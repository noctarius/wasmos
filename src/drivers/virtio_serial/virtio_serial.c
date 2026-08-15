/* virtio_serial.c - VirtIO serial port WASM driver.
 * Finds the VirtIO serial PCI function by config-space scan and registers the
 * "virtio.serial" service, which answers device-identity queries and reads or
 * writes 32-bit registers within the device's I/O window
 * (VIRTIO_SERIAL_IPC_QUERY_REQ / _READ_REG32_REQ / _WRITE_REG32_REQ).
 * Discovery and register access only: there is no queue setup and no data
 * plane yet — see the TODO in initialize(). */
#include <stdint.h>
#include <stdio.h>
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

/* PCI configuration mechanism #1: write a (bus, device, function, offset)
 * address to 0xCF8, then read or write the 32-bit datum at 0xCFC. Fixed by the
 * PCI Local Bus specification. */
#define PCI_CFG_ADDR_PORT 0xCF8
#define PCI_CFG_DATA_PORT 0xCFC

/* PCI identity accepted as a virtio serial port. 0x1AF4 is the Red Hat / virtio
 * vendor id; 0x1003 is the transitional console device and 0x1043 the modern
 * one (0x1040 + virtio device type 3).
 *
 * VIRTIO_PCI_DEV_MIN/MAX span the WHOLE virtio device-id range, and the match
 * accepts anything inside it in addition to the two exact ids -- so this probe
 * will also claim a virtio device of some other type if it appears first. That
 * is tolerable only because the driver does discovery and register access and
 * has no data plane; it would not be once queues are set up. */
#define VIRTIO_PCI_VENDOR_ID 0x1AF4u
#define VIRTIO_PCI_DEV_MIN 0x1000u
#define VIRTIO_PCI_DEV_MAX 0x107Fu
#define VIRTIO_SERIAL_DEV_LEGACY 0x1003u
#define VIRTIO_SERIAL_DEV_TRANSITIONAL 0x1043u

typedef struct {
    uint8_t present;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t irq;
    uint16_t io_base;
    uint16_t vendor_id;
    uint16_t device_id;
} virtio_serial_device_t;

static int32_t g_endpoint = -1;
static virtio_serial_device_t g_dev;

static uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t reg) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                       ((uint32_t)function << 8) | ((uint32_t)reg & 0xFCu);
    (void)wasmos_io_out32(PCI_CFG_ADDR_PORT, (int32_t)address);
    uint32_t value = 0xFFFFFFFFu; /* an absent device reads back all-ones */
    (void)wasmos_io_in32(PCI_CFG_DATA_PORT, &value);
    return value;
}

static int is_virtio_serial_device(uint16_t vendor_id, uint16_t device_id) {
    if (vendor_id != VIRTIO_PCI_VENDOR_ID) {
        return 0;
    }
    if (device_id == VIRTIO_SERIAL_DEV_LEGACY || device_id == VIRTIO_SERIAL_DEV_TRANSITIONAL) {
        return 1;
    }
    return device_id >= VIRTIO_PCI_DEV_MIN && device_id <= VIRTIO_PCI_DEV_MAX;
}

static int probe_virtio_serial(void) {
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            for (uint8_t function = 0; function < 8; ++function) {
                uint32_t id = pci_config_read32((uint8_t)bus, slot, function, 0x00);
                uint16_t vendor_id = (uint16_t)(id & 0xFFFFu);
                uint16_t device_id = (uint16_t)((id >> 16) & 0xFFFFu);
                if (vendor_id == 0xFFFFu) {
                    if (function == 0u) {
                        break;
                    }
                    continue;
                }
                if (!is_virtio_serial_device(vendor_id, device_id)) {
                    continue;
                }
                uint32_t class_reg = pci_config_read32((uint8_t)bus, slot, function, 0x08);
                uint8_t class_code = (uint8_t)((class_reg >> 24) & 0xFFu);
                uint8_t subclass = (uint8_t)((class_reg >> 16) & 0xFFu);
                if (class_code != 0x07u || subclass != 0x00u) {
                    continue;
                }
                uint32_t bar0 = pci_config_read32((uint8_t)bus, slot, function, 0x10);
                if ((bar0 & 0x1u) == 0u) {
                    continue;
                }
                uint16_t io_base = (uint16_t)(bar0 & 0xFFFCu);
                if (io_base == 0u) {
                    continue;
                }
                uint8_t irq =
                    (uint8_t)(pci_config_read32((uint8_t)bus, slot, function, 0x3C) & 0xFFu);
                g_dev.present = 1;
                g_dev.bus = (uint8_t)bus;
                g_dev.slot = slot;
                g_dev.function = function;
                g_dev.irq = irq;
                g_dev.io_base = io_base;
                g_dev.vendor_id = vendor_id;
                g_dev.device_id = device_id;
                return 0;
            }
        }
    }
    return -1;
}

/* FIXME: the `code` values passed in below are bare errno-like numbers, not
 * packed WASMOS_ERR_* codes, so a peer cannot decode them with wasmos_strerror.
 * A virtio-serial domain in abi/errors.yaml is what this needs. */
static void send_error(int32_t dest, int32_t request_id, int32_t code) {
    (void)wasmos_ipc_send(dest, g_endpoint, VIRTIO_SERIAL_IPC_ERROR, request_id, code, 0, 0, 0);
}

static void handle_query(int32_t source, int32_t request_id) {
    int32_t present = g_dev.present ? 1 : 0;
    int32_t packed0 = ((int32_t)g_dev.vendor_id << 16) | (int32_t)g_dev.device_id;
    int32_t packed1 = ((int32_t)g_dev.bus << 24) | ((int32_t)g_dev.slot << 16) |
                      ((int32_t)g_dev.function << 8) | (int32_t)g_dev.irq;
    (void)wasmos_ipc_send(source, g_endpoint, VIRTIO_SERIAL_IPC_RESP, request_id, present, packed0,
                          packed1, (int32_t)g_dev.io_base);
}

/* Register access is confined to the first 64 bytes of the device's I/O window
 * (the legacy virtio register block) and must be dword-aligned. */
static void handle_read_reg32(int32_t source, int32_t request_id, int32_t offset) {
    if (!g_dev.present) {
        send_error(source, request_id, -2);
        return;
    }
    if (offset < 0 || offset > 0x3Cu || (offset & 0x3) != 0) {
        send_error(source, request_id, -22);
        return;
    }
    uint32_t value = 0;
    (void)wasmos_io_in32((int32_t)((uint32_t)g_dev.io_base + (uint32_t)offset), &value);
    (void)wasmos_ipc_send(source, g_endpoint, VIRTIO_SERIAL_IPC_RESP, request_id, 0, (int32_t)value,
                          offset, 0);
}

static void handle_write_reg32(int32_t source, int32_t request_id, int32_t offset, int32_t value) {
    if (!g_dev.present) {
        send_error(source, request_id, -2);
        return;
    }
    if (offset < 0 || offset > 0x3Cu || (offset & 0x3) != 0) {
        send_error(source, request_id, -22);
        return;
    }
    (void)wasmos_io_out32((int32_t)((uint32_t)g_dev.io_base + (uint32_t)offset), value);
    (void)wasmos_ipc_send(source, g_endpoint, VIRTIO_SERIAL_IPC_RESP, request_id, 0, offset, value,
                          0);
}

/* Driver entry point: probe the virtio-serial PCI function, register, and serve
 * the query / register-read / register-write requests forever.
 *
 * All four parameters are ignored; proc_endpoint is overwritten from the
 * spawn-info contract, because the entry arguments are passed as zero. Probing
 * is allowed to find nothing -- the driver still registers and answers queries
 * reporting the device as absent, rather than failing to start.
 *
 * On success this does not return.
 * TODO: the bring-up failure paths return a bare -1, and the unknown-request
 * path replies with a bare -38, rather than packed abi/errors.yaml codes. */
WASMOS_WASM_EXPORT int32_t initialize(void) {
    /* proc.endpoint comes from the spawn-info contract, not an entry arg. */
    int32_t proc_endpoint = wasmos_startup_proc_endpoint();
    if (proc_endpoint < 0) {
        return -1;
    }

    g_endpoint = wasmos_ipc_create_endpoint();
    if (g_endpoint < 0) {
        return -1;
    }
    g_dev.present = 0;
    (void)probe_virtio_serial();

    /* TODO(virtio-serial-transport): add queue setup and data/control-plane
     * operations; this initial slice is discovery + register access only. */
    if (wasmos_svc_register(proc_endpoint, g_endpoint, "virtio.serial", 1) != 0) {
        (void)printf("[virtio-serial] register failed\n");
        return -1;
    }
    if (g_dev.present) {
        (void)printf("[virtio-serial] ready io=0x%04X irq=%u dev=%04X\n", (unsigned)g_dev.io_base,
                     (unsigned)g_dev.irq, (unsigned)g_dev.device_id);
    } else {
        (void)printf("[virtio-serial] no device found\n");
    }
    wasmos_sys_notify_ready(proc_endpoint, g_endpoint);

    for (;;) {
        if (wasmos_ipc_select_one(g_endpoint) != 1) {
            continue;
        }
        wasmos_ipc_message_t msg;
        wasmos_ipc_message_read_last(&msg);
        int32_t type = msg.type;
        int32_t req = msg.request_id;
        int32_t source = msg.source;
        int32_t arg0 = msg.arg0;
        int32_t arg1 = msg.arg1;
        if (source < 0) {
            continue;
        }
        if (type == VIRTIO_SERIAL_IPC_QUERY_REQ) {
            handle_query(source, req);
        } else if (type == VIRTIO_SERIAL_IPC_READ_REG32_REQ) {
            handle_read_reg32(source, req, arg0);
        } else if (type == VIRTIO_SERIAL_IPC_WRITE_REG32_REQ) {
            handle_write_reg32(source, req, arg0, arg1);
        } else {
            send_error(source, req, -38);
        }
    }
    return 0;
}
