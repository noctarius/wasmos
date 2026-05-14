#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos_driver_abi.h"

#define PCI_CFG_ADDR_PORT 0xCF8
#define PCI_CFG_DATA_PORT 0xCFC

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
               uint8_t bus,
               uint8_t device,
               uint8_t function,
               uint8_t class_code,
               uint8_t subclass,
               uint8_t prog_if,
               uint16_t vendor_id,
               uint16_t device_id,
               int32_t request_id)
{
    uint32_t arg0 = ((uint32_t)bus << 24) |
                    ((uint32_t)device << 16) |
                    ((uint32_t)function << 8) |
                    (uint32_t)class_code;
    uint32_t arg1 = ((uint32_t)subclass << 24) |
                    ((uint32_t)prog_if << 16) |
                    (uint32_t)vendor_id;
    uint32_t arg2 = (uint32_t)device_id;
    (void)wasmos_ipc_send(devmgr_endpoint,
                          source_endpoint,
                          DEVMGR_PUBLISH_DEVICE,
                          request_id,
                          (int32_t)arg0,
                          (int32_t)arg1,
                          (int32_t)arg2,
                          0);
}

WASMOS_WASM_EXPORT int32_t
initialize(int32_t devmgr_endpoint,
           int32_t ignored_arg1,
           int32_t ignored_arg2,
           int32_t ignored_arg3)
{
    (void)ignored_arg1;
    (void)ignored_arg2;
    (void)ignored_arg3;
    if (devmgr_endpoint < 0) {
        return -1;
    }

    int32_t source_endpoint = wasmos_ipc_create_endpoint();
    if (source_endpoint < 0) {
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
                uint16_t device_id = (uint16_t)((id_reg >> 16) & 0xFFFFu);
                uint32_t class_reg = pci_config_read32((uint8_t)bus, device, function, 0x08);
                uint8_t class_code = (uint8_t)((class_reg >> 24) & 0xFFu);
                uint8_t subclass = (uint8_t)((class_reg >> 16) & 0xFFu);
                uint8_t prog_if = (uint8_t)((class_reg >> 8) & 0xFFu);
                publish_record(devmgr_endpoint,
                               source_endpoint,
                               (uint8_t)bus,
                               device,
                               function,
                               class_code,
                               subclass,
                               prog_if,
                               vendor_id,
                               device_id,
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
    return 0;
}
