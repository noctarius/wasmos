/* pci_config.h - PCI configuration space access (mechanism 1, ports 0xCF8/0xCFC)
 * and the quiesce of a dead driver's function.
 *
 * The kernel is the only process that touches the two ports. An access is a
 * write of the address port followed by an access of the data port, so two
 * writers interleaving would each reach the other's register: pci-bus reaches
 * config space through the pci_config_read / pci_config_write host calls
 * (abi/hostcalls.yaml), which serialise on the same lock as the quiesce.
 *
 * A function is named by its routing id, bdf = bus << 8 | device << 3 |
 * function. Registers are 32-bit and dword-aligned; reg & 0xFC is used. */
#ifndef WASMOS_PCI_CONFIG_H
#define WASMOS_PCI_CONFIG_H

#include <stdint.h>

/* Reads the dword at `reg`. An absent function reads all-ones. */
uint32_t pci_config_read32(uint16_t bdf, uint8_t reg);

/* Writes the dword at `reg`. */
void pci_config_write32(uint16_t bdf, uint8_t reg, uint32_t value);

/* Stops a function from reaching memory or raising interrupts on its own:
 * clears Bus Master Enable, sets INTx Disable, and clears the enable bit of its
 * MSI and MSI-X capabilities, then reads the command register back so the
 * writes have reached the device. Status bits are written as zero, which
 * leaves their write-1-to-clear state alone. Returns 1 when the function is
 * present and was quiesced, 0 when it reads as absent.
 *
 * What this does not undo is the device's own state: its queues still name the
 * old frames, so the next driver must reset the device before it turns bus
 * mastering back on (an MSI bind through pci-bus does). */
int pci_quiesce_function(uint16_t bdf);

/* Quiesces the PCI function context_id's spawn profile names
 * (capability_pci_function), if any, and logs it. The reap path calls this
 * before it releases the context's DMA regions and borrows, so no frame the
 * device could still write is recycled while it can. */
void pci_quiesce_context(uint32_t context_id, uint32_t pid);

#endif
