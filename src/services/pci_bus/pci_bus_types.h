/* pci_bus_types.h - PCI enumeration record and I/O port constants */
#ifndef WASMOS_PCI_BUS_TYPES_H
#define WASMOS_PCI_BUS_TYPES_H

#include <stdint.h>

/* Standard x86 PCI configuration mechanism 1 I/O ports. */
#define PCI_CFG_ADDR_PORT 0xCF8
#define PCI_CFG_DATA_PORT 0xCFC

/* One enumerated PCI function with the fields the device-manager needs. */
typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t io_port_base;  /* BAR0 I/O base (0 if MMIO or absent) */
    uint8_t mmio_hint;      /* 1 if BAR0 looks like a 32-bit MMIO BAR */
    uint8_t irq_hint;       /* interrupt line from config register 0x3C */
} pci_device_record_t;

#endif
