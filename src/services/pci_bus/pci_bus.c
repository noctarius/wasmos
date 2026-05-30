#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos_driver_abi.h"
#include "pci_bus_types.h"

static void
log_record(const pci_device_record_t *rec)
{
    if (!rec) {
        return;
    }
    (void)printf("[pci-bus] dev %02X:%02X.%02X class %02X:%02X:%02X vid:did %04X:%04X mmio %02X irq %02X\n",
                 (unsigned)rec->bus,
                 (unsigned)rec->device,
                 (unsigned)rec->function,
                 (unsigned)rec->class_code,
                 (unsigned)rec->subclass,
                 (unsigned)rec->prog_if,
                 (unsigned)rec->vendor_id,
                 (unsigned)rec->device_id,
                 (unsigned)rec->mmio_hint,
                 (unsigned)rec->irq_hint);
}

static uint32_t
pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t reg)
{
    uint32_t address = 0x80000000u |
                       ((uint32_t)bus << 16) |
                       ((uint32_t)device << 11) |
                       ((uint32_t)function << 8) |
                       ((uint32_t)reg & 0xFCu);
    (void)wasmos_io_out32(PCI_CFG_ADDR_PORT, (int32_t)address);
    return (uint32_t)wasmos_io_in32(PCI_CFG_DATA_PORT);
}

static void
publish_record(int32_t devmgr_endpoint,
               int32_t source_endpoint,
               const pci_device_record_t *rec,
               int32_t request_id)
{
    if (!rec) {
        return;
    }
    uint32_t arg0 = ((uint32_t)rec->bus << 24) |
                    ((uint32_t)rec->device << 16) |
                    ((uint32_t)rec->function << 8) |
                    (uint32_t)rec->class_code;
    uint32_t arg1 = ((uint32_t)rec->subclass << 24) |
                    ((uint32_t)rec->prog_if << 16) |
                    (uint32_t)rec->vendor_id;
    uint32_t arg2 = (uint32_t)rec->device_id;
    uint32_t arg3 = ((uint32_t)rec->irq_hint << 8) | (uint32_t)rec->mmio_hint;
    (void)wasmos_ipc_send(devmgr_endpoint,
                          source_endpoint,
                          DEVMGR_PUBLISH_DEVICE,
                          request_id,
                          (int32_t)arg0,
                          (int32_t)arg1,
                          (int32_t)arg2,
                          (int32_t)arg3);
}

WASMOS_WASM_EXPORT int32_t
initialize(int32_t proc_endpoint,
           int32_t ignored_arg1,
           int32_t ignored_arg2,
           int32_t ignored_arg3)
{
    (void)ignored_arg1;
    (void)ignored_arg2;
    (void)ignored_arg3;
    if (proc_endpoint < 0) {
        return -1;
    }

    int32_t source_endpoint = wasmos_ipc_create_endpoint();
    if (source_endpoint < 0) {
        return -1;
    }
    int32_t devmgr_endpoint = wasmos_sys_svc_lookup_retry(proc_endpoint,
                                                          source_endpoint,
                                                          "devmgr.inv",
                                                          1,
                                                          1024);
    if (devmgr_endpoint == -1) {
        return -1;
    }
    int32_t request_id = 1;
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t device = 0; device < 32; ++device) {
            for (uint8_t function = 0; function < 8; ++function) {
                uint32_t id_reg = pci_config_read32((uint8_t)bus, device, function, 0x00);
                uint16_t vendor_id = (uint16_t)(id_reg & 0xFFFFu);
                if (vendor_id == 0xFFFFu) {
                    if (function == 0) {
                        break;
                    }
                    continue;
                }
                pci_device_record_t rec;
                rec.bus = (uint8_t)bus;
                rec.device = device;
                rec.function = function;
                rec.vendor_id = vendor_id;
                rec.device_id = (uint16_t)((id_reg >> 16) & 0xFFFFu);
                uint32_t class_reg = pci_config_read32((uint8_t)bus, device, function, 0x08);
                rec.class_code = (uint8_t)((class_reg >> 24) & 0xFFu);
                rec.subclass = (uint8_t)((class_reg >> 16) & 0xFFu);
                rec.prog_if = (uint8_t)((class_reg >> 8) & 0xFFu);
                uint32_t bar0 = pci_config_read32((uint8_t)bus, device, function, 0x10);
                rec.mmio_hint = ((bar0 & 0x1u) == 0u && (bar0 & 0xFFFFFFF0u) != 0u) ? 1u : 0u;
                uint32_t irq_reg = pci_config_read32((uint8_t)bus, device, function, 0x3C);
                rec.irq_hint = (uint8_t)(irq_reg & 0xFFu);
                log_record(&rec);
                publish_record(devmgr_endpoint,
                               source_endpoint,
                               &rec,
                               request_id++);
                uint32_t header_reg = pci_config_read32((uint8_t)bus, device, 0, 0x0C);
                if (function == 0 && (((header_reg >> 16) & 0x80u) == 0)) {
                    break;
                }
            }
        }
    }

    (void)wasmos_ipc_send(devmgr_endpoint,
                          source_endpoint,
                          DEVMGR_PCI_SCAN_DONE,
                          request_id,
                          0,
                          0,
                          0,
                          0);
    wasmos_sys_notify_ready(proc_endpoint, source_endpoint);
    return 0;
}
