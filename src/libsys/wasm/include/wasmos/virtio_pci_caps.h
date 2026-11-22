/* virtio_pci_caps.h - locating a virtio-modern device's register blocks.
 *
 * A virtio 1.x PCI device describes where its registers live through
 * vendor-specific capabilities (cap id 0x09) in its config-space capability
 * list (virtio 1.2 §4.1.4): each names a structure type, the BAR holding it, and
 * the offset and length inside that BAR. This header only PARSES that list,
 * through a caller-supplied config read, so it has no imports and is
 * host-testable; the transport that uses the result is virtio_modern.h. */
#ifndef WASMOS_VIRTIO_PCI_CAPS_H
#define WASMOS_VIRTIO_PCI_CAPS_H

#include <stdint.h>

/* virtio_pci_cap.cfg_type (virtio 1.2 §4.1.4). */
#define VIRTIO_PCI_CAP_COMMON_CFG 1u
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2u
#define VIRTIO_PCI_CAP_ISR_CFG 3u
#define VIRTIO_PCI_CAP_DEVICE_CFG 4u

/* One structure's place: `length` bytes at `offset` inside BAR `bar`. */
typedef struct {
    uint8_t bar;
    uint32_t offset;
    uint32_t length;
} virtio_pci_cap_loc_t;

/* Where a device's structures live. `found` has bit (1 << cfg_type) set for
 * each type located. notify_off_multiplier scales a queue's queue_notify_off
 * into a byte offset inside the notify structure. */
typedef struct {
    virtio_pci_cap_loc_t common;
    virtio_pci_cap_loc_t notify;
    virtio_pci_cap_loc_t isr;
    virtio_pci_cap_loc_t device;
    uint32_t notify_off_multiplier;
    uint32_t found;
} virtio_pci_layout_t;

/* Reads the config-space dword at the dword-aligned register `reg` into *out;
 * returns 0, or non-zero when the read is refused. */
typedef int32_t (*virtio_pci_cfg_read32_fn)(void* user, uint32_t reg, uint32_t* out);

/* Walks the capability list through `read` and records the first structure of
 * each type (the spec lets a device offer several; the first is the preferred
 * one). Structures of a type this header does not know are skipped. Returns 0
 * with *out filled when the common, notify and ISR structures were all found,
 * otherwise -1 -- including for a device with no capability list, a refused
 * read, or a list that loops. */
static inline int32_t virtio_pci_find_layout(virtio_pci_cfg_read32_fn read, void* user,
                                             virtio_pci_layout_t* out) {
    const uint32_t needed = (1u << VIRTIO_PCI_CAP_COMMON_CFG) | (1u << VIRTIO_PCI_CAP_NOTIFY_CFG) |
                            (1u << VIRTIO_PCI_CAP_ISR_CFG);
    uint32_t dword = 0;
    uint32_t ptr;
    if (!read || !out) {
        return -1;
    }
    for (uint32_t i = 0; i < sizeof(*out); ++i) {
        ((uint8_t*)out)[i] = 0;
    }
    /* Status register bit 4: the function has a capability list at 0x34. */
    if (read(user, 0x04u, &dword) != 0 || (dword & (1u << 20)) == 0u ||
        read(user, 0x34u, &dword) != 0) {
        return -1;
    }
    ptr = dword & 0xFCu;
    /* 48 hops covers every capability a 256-byte config space can hold, so a
     * list that points back at itself ends here instead of spinning. */
    for (uint32_t hops = 0; ptr >= 0x40u && hops < 48u; ++hops) {
        uint32_t head = 0;
        if (read(user, ptr, &head) != 0) {
            return -1;
        }
        uint32_t id = head & 0xFFu;
        uint32_t type = (head >> 24) & 0xFFu;
        if (id == 0x09u && type >= VIRTIO_PCI_CAP_COMMON_CFG && type <= VIRTIO_PCI_CAP_DEVICE_CFG &&
            (out->found & (1u << type)) == 0u) {
            uint32_t bar = 0;
            uint32_t offset = 0;
            uint32_t length = 0;
            if (read(user, ptr + 4u, &bar) != 0 || read(user, ptr + 8u, &offset) != 0 ||
                read(user, ptr + 12u, &length) != 0) {
                return -1;
            }
            virtio_pci_cap_loc_t loc = {(uint8_t)(bar & 0xFFu), offset, length};
            if (type == VIRTIO_PCI_CAP_COMMON_CFG) {
                out->common = loc;
            } else if (type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                uint32_t mult = 0;
                if (read(user, ptr + 16u, &mult) != 0) {
                    return -1;
                }
                out->notify = loc;
                out->notify_off_multiplier = mult;
            } else if (type == VIRTIO_PCI_CAP_ISR_CFG) {
                out->isr = loc;
            } else {
                out->device = loc;
            }
            out->found |= 1u << type;
        }
        ptr = (head >> 8) & 0xFCu;
    }
    return (out->found & needed) == needed ? 0 : -1;
}

#endif
