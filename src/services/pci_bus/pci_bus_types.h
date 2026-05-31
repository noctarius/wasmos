#ifndef WASMOS_PCI_BUS_TYPES_H
#define WASMOS_PCI_BUS_TYPES_H

#include <stdint.h>

#define PCI_CFG_ADDR_PORT 0xCF8
#define PCI_CFG_DATA_PORT 0xCFC

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t io_port_base;
    uint8_t mmio_hint;
    uint8_t irq_hint;
} pci_device_record_t;

#endif
