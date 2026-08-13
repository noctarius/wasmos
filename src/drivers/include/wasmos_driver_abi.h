/* wasmos_driver_abi.h - Shared IPC message type constants used by all drivers and services.
 *
 * The opcodes themselves are generated from abi/opcodes.yaml (included below);
 * that file is the authority for which subsystem owns which range and for the
 * per-opcode argument encodings.  Ranges are scoped to a protocol, not globally
 * unique: gfx reuses 0x200–0x2FF alongside the process manager, and font reuses
 * 0xA00–0xAFF alongside the network-adapter drivers, so a message type is only
 * meaningful together with the endpoint it was sent to.
 *
 * Within a subsystem, request/response pairs follow REQ = base, RESP = base+0x80,
 * and a subsystem that declares an error opcode puts it at the top of its own
 * range (0x1FF for chardev, 0x8BF for virtio-serial, ...).  Message fields
 * (type, request_id, source, destination, arg0..arg3) match the ipc_message_t
 * layout in the kernel. */
#ifndef WASMOS_DRIVER_ABI_H
#define WASMOS_DRIVER_ABI_H

#include <stdint.h>
#include "../../../abi/generated/c/wasmos_opcodes.h" /* generated opcode enums (abi/opcodes.yaml) */
#include "../../../abi/generated/c/wasmos_status.h"  /* packed error codes (abi/errors.yaml) */

#ifndef WASMOS_CONSOLE_RING_SHARED_H
#define WASMOS_CONSOLE_RING_SHARED_H
#define CONSOLE_RING_DATA_SIZE 4080u

typedef struct {
    volatile uint32_t write_pos;
    volatile uint32_t read_pos;
    uint32_t capacity;
    uint32_t _pad;
    uint8_t data[CONSOLE_RING_DATA_SIZE];
} console_ring_t;
#endif

enum { PROC_MODULE_SOURCE_INITFS = 0, PROC_MODULE_SOURCE_FS = 1 };

/* arg0 flags for PROC_IPC_SPAWN_PATH (request). */
#define PROC_SPAWN_PATH_FLAG_DETACH (1u << 0) /* skip ready-wait even for service/driver */
/* Reap the child's process slot automatically when it exits, instead of leaving
 * it as a zombie until a waiter consumes its status.  Orthogonal to DETACH
 * (which is about ready-gating at spawn): set this for fire-and-forget one-shot
 * children that nobody PROC_IPC_WAITs on (bus enumerators, boot self-tests, …).
 * Must NOT be set when the spawner will PROC_IPC_WAIT for the exit status. */
#define PROC_SPAWN_PATH_FLAG_AUTOREAP (1u << 1)

/* Process-manager failure reasons: spawn paths use WASMOS_ERR_PROC_SPAWN_*
 * (domain 1), non-path PM IPC uses WASMOS_ERR_PROC_PM_* (domain 2), carried in
 * PROC_IPC_ERROR.arg1 so a failed op reports WHY. Codes are negative; decode
 * arg1 with wasmos_error_* / wasmos_strerror. (From wasmos_status.h, above.) */

/* shmem map/map_auto failure reasons: the host calls return WASMOS_ERR_SHMEM_*
 * (domain 3, see abi/errors.yaml / abi/generated/c/wasmos_status.h). Codes are
 * negative; decode the return value with wasmos_error_* / wasmos_strerror. */

/* Filesystem failure reasons: the FAT backend returns WASMOS_ERR_FS_* (domain 4,
 * abi/errors.yaml) in FS_IPC_ERROR / FS_IPC_RESP arg0, so a failed FS op reports
 * WHY; fs-manager relays arg0 unchanged. Codes are negative; decode arg0 with
 * wasmos_error_* / wasmos_strerror. (From wasmos_status.h, included above.) */

/* Flags returned in arg1 of PROC_IPC_RESP for PROC_IPC_SPAWN_PATH.
 * Mirror of WASMOS_APP_FLAG_* in the kernel's wasmos_app.h. */
#define WASMOS_SPAWN_FLAG_DRIVER (1u << 0)
#define WASMOS_SPAWN_FLAG_SERVICE (1u << 1)
#define WASMOS_SPAWN_FLAG_APP (1u << 2)

#define WASMOS_BROKER_SPAWN_PLAN_VERSION 1u

#define WASMOS_SUBSYSTEM_TAG_LEN 8u
#define WASMOS_EXEC_HANDLER_NAME_LEN 32u
#define WASMOS_EXEC_MATCH_TEXT_LEN 32u
#define WASMOS_EXEC_MATCH_MAX_BYTES 16u
#define WASMOS_EXEC_MATCH_MAX_NODES 16u

typedef enum {
    WASMOS_EXEC_MATCH_PREFIX = 0,
    WASMOS_EXEC_MATCH_EXTENSION = 1,
    WASMOS_EXEC_MATCH_FILENAME = 2,
    WASMOS_EXEC_MATCH_AND = 3,
    WASMOS_EXEC_MATCH_OR = 4,
    WASMOS_EXEC_MATCH_NOT = 5,
} wasmos_exec_match_kind_t;

typedef struct {
    wasmos_exec_match_kind_t kind;
    uint16_t left_index;
    uint16_t right_index;
    uint8_t value_len;
    union {
        uint8_t prefix[WASMOS_EXEC_MATCH_MAX_BYTES];
        char text[WASMOS_EXEC_MATCH_TEXT_LEN + 1];
    } value;
} wasmos_exec_match_node_t;

enum {
    WASMOS_BROKER_PLAN_KIND_NONE = 0,
    /* Broker resolved the guest workload to a built-in `.wap` path that PM
     * can later launch through the ordinary path-based spawn flow. */
    WASMOS_BROKER_PLAN_KIND_WAP_PATH = 1
};

typedef struct __attribute__((packed)) {
    uint32_t version;
    uint32_t spawn_flags;
    uint32_t blob_offset;
    uint32_t blob_size;
    uint32_t path_offset;
    uint32_t path_len;
    uint32_t args_offset;
    uint32_t args_len;
    uint32_t handler_name_offset;
    uint32_t handler_name_len;
    char request_tag[9];
    char runtime_tag[9];
    char broker_name[9];
} wasmos_broker_spawn_plan_request_t;

typedef struct __attribute__((packed)) {
    uint32_t version;
    uint32_t plan_kind;
    uint32_t plan_flags;
    uint32_t host_path_offset;
    uint32_t host_path_len;
    uint32_t host_args_offset;
    uint32_t host_args_len;
    char request_tag[9];
    char runtime_tag[9];
} wasmos_broker_spawn_plan_response_t;

#define WASMOS_SUBSYSTEM_REGISTER_BROKER_DESC_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t version;
    uint32_t broker_endpoint;
    uint32_t flags;
    uint8_t uses_wasm_payload;
    uint8_t needs_runtime_lock;
    uint8_t gates_ready_for_services;
    uint8_t reserved0;
    char request_tag[WASMOS_SUBSYSTEM_TAG_LEN + 1];
    char runtime_tag[WASMOS_SUBSYSTEM_TAG_LEN + 1];
    char broker_name[WASMOS_SUBSYSTEM_TAG_LEN + 1];
} wasmos_subsystem_broker_register_desc_t;

#define WASMOS_EXEC_HANDLER_REGISTER_DESC_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t version;
    uint32_t priority;
    uint32_t max_probe_bytes;
    uint32_t node_count;
    uint32_t root_index;
    char request_tag[WASMOS_SUBSYSTEM_TAG_LEN + 1];
    char handler_name[WASMOS_EXEC_HANDLER_NAME_LEN + 1];
} wasmos_exec_handler_register_desc_t;

#define WASMOS_SVC_REGISTER_DESC_VERSION 2u
#define WASMOS_SVC_NAME_MAX 36u
#define WASMOS_SVC_CLASS_MAX 16u /* incl. NUL; keep == SVC_CLASS_NAME_MAX */

/* Existence-event kinds carried in SVC_IPC_CLASS_EVENT arg0. Keep in sync with
 * SVC_CLASS_EVENT_* in src/kernel/include/service_class_registry.h. */
#define SVC_CLASS_EVENT_ADD 1u
#define SVC_CLASS_EVENT_REMOVE 2u

/* Register descriptor written to the xfer buffer for SVC_IPC_REGISTER_DESC_REQ.
 * Extensible: bump WASMOS_SVC_REGISTER_DESC_VERSION and append fields.  Mirror
 * this layout in any non-C binding that registers services.  v2 appended the
 * class/instance fields; PM accepts a v1-length descriptor (no class) for
 * back-compat by checking the byte length, not just the version. */
typedef struct {
    uint32_t version;               /* = WASMOS_SVC_REGISTER_DESC_VERSION */
    uint32_t service_endpoint;      /* endpoint clients send requests to */
    uint32_t flags;                 /* reserved, 0 */
    char name[WASMOS_SVC_NAME_MAX]; /* NUL-terminated service name */
    /* v2+ (present iff the descriptor byte length covers these fields): */
    uint32_t instance;                     /* provider instance index within the class */
    char class_name[WASMOS_SVC_CLASS_MAX]; /* NUL-term; "" = no class */
} svc_register_desc_t;

/* v1 descriptor length: fields up to and including name[], no class/instance. */
#define WASMOS_SVC_REGISTER_DESC_V1_BYTES (3u * (uint32_t)sizeof(uint32_t) + WASMOS_SVC_NAME_MAX)

/* One resolved provider returned by SVC_IPC_LOOKUP_CLASS_REQ (wire layout;
 * matches service_class_provider_t). */
typedef struct {
    uint32_t instance;
    uint32_t endpoint;
    uint32_t pid;
} svc_class_entry_t;

enum { PROC_STATUS_UNKNOWN = 0, PROC_STATUS_RUNNING = 1, PROC_STATUS_ZOMBIE = 2 };

/* Virtual class FS backends register under. Class instances must be unique per
 * provider, so backends encode (kind, unit) into the registry instance while
 * still reporting the plain kind over FSMGR_IPC_BACKEND_INFO_RESP arg0. */
#define FSMGR_BACKEND_CLASS "fs.backend"

enum { FSMGR_BACKEND_BOOT = 1, FSMGR_BACKEND_INIT = 2 };

#define FSMGR_BACKEND_INSTANCE(kind, unit) ((((uint32_t)(kind)) << 8) | ((uint32_t)(unit) & 0xFFu))

/* One cell in a shared FBTEXT_IPC_BLIT grid buffer.  Layout is identical to the
 * framebuffer driver's fbtext_cell_t and the VT's vt_cell_t (8 bytes), so both
 * sides copy grids without per-cell conversion. */
typedef struct {
    uint32_t ch;  /* Unicode codepoint; 0 or ' ' = blank */
    uint8_t fg;   /* 4-bit foreground palette index */
    uint8_t bg;   /* 4-bit background palette index */
    uint8_t attr; /* reserved (bold/underline/blink) */
    uint8_t _pad;
} fbtext_blit_cell_t;

enum { FBTEXT_CAP_SET_RESOLUTION = 1u << 0, FBTEXT_CAP_QUERY_MODES = 1u << 1 };

/* Serial driver subscription: a client (the vt service) registers to receive
 * COM1 RX bytes as VT_IPC_SERIAL_INPUT_REQ pushes.  Mirrors the keyboard
 * driver's subscribe/notify pattern. */

enum { VT_INPUT_MODE_RAW = 0, VT_INPUT_MODE_CANONICAL = 1 << 0, VT_INPUT_MODE_ECHO = 1 << 1 };

enum {
    DEVMGR_CAP_IO_PORT = 1 << 0,
    DEVMGR_CAP_MMIO_MAP = 1 << 1,
    DEVMGR_CAP_IRQ = 1 << 2,
    DEVMGR_CAP_DMA = 1 << 3
};

/* Generic hardware-RNG service protocol (virtual class "hrng"). Backend-neutral:
 * any entropy source (virtio-rng, RDRAND, ...) registers under class "hrng" and
 * speaks this. A client stages an output buffer via the xfer/FS-buffer API and
 * asks the provider to fill up to `len` bytes of entropy into it; libc layers
 * random-int / random-byte-array helpers on top of GET_BYTES. */

/* Status codes are the packed hrng domain in abi/errors.yaml:
 * WASMOS_ERR_NONE (0) on success, else a negative WASMOS_ERR_HRNG_*. */

/* Largest single GET_BYTES request the provider fills in one round-trip (its DMA
 * pool is one page). Clients wanting more loop. */
#define HRNG_MAX_BYTES_PER_REQ 4096u

/* Status codes are the packed net domain in abi/errors.yaml:
 * WASMOS_ERR_NONE (0) on success, else a negative WASMOS_ERR_NET_*. */

/* Socket control-plane constants. Socket payload is never carried by IPC:
 * SOCKET_OPEN arg0=descriptor buffer_id, arg1=descriptor borrow_id,
 * arg2=descriptor bytes, arg3=0. The descriptor transfers persistent grants
 * for the client-owned TX/RX SPSC rings, while TX/RX_NOTIFY are
 * empty-to-non-empty doorbells. */
enum {
    NET_SOCKET_AF_INET = 2,
    NET_SOCKET_AF_INET6 = 10,
    NET_SOCKET_STREAM = 1,
    NET_SOCKET_DGRAM = 2
};

#define NET_SOCKET_OPEN_DESCRIPTOR_VERSION 1u

/* net_socket_open_descriptor_v1_t.flags bits. */
#define NET_SOCKET_OPEN_FLAG_TLS 1u /* stream socket is wrapped in TLS (altcp_tls) */

/* Maximum SNI / certificate-verification hostname carried in the open descriptor
 * (a DNS name is at most 253 bytes; the extra room leaves space for the NUL that
 * mbedtls_ssl_set_hostname needs). Milestone C: for a TLS socket this is the name
 * checked against the server certificate CN/SAN and sent in the SNI extension. */
#define NET_SOCKET_SNI_MAX 256u

/* Datagram ring records carry endpoint metadata as well as payload. A client
 * writes destination fields for an unconnected sendto; RX records contain the
 * source fields supplied by lwIP. Connected sockets may leave destination at
 * zero and use their connected peer. */
#define NET_UDP_DATAGRAM_RECORD_VERSION 1u
#define NET_UDP_DATAGRAM_FLAG_DESTINATION 1u

typedef struct __attribute__((packed)) {
    uint16_t version;
    uint16_t flags;
    uint32_t addr_v4;
    uint16_t port;
    uint16_t payload_bytes;
} net_udp_datagram_record_v1_t;

typedef struct __attribute__((packed)) {
    uint16_t version;
    uint16_t bytes;
    uint32_t family;
    uint32_t type;
    uint32_t stack_id;
    uint32_t flags;
    uint32_t tx_buffer_id;
    uint32_t tx_borrow_id;
    uint32_t tx_bytes;
    uint32_t rx_buffer_id;
    uint32_t rx_borrow_id;
    uint32_t rx_bytes;
    /* SNI / certificate-verification hostname for a TLS stream socket (milestone
     * C). sni_len is the byte count (<= NET_SOCKET_SNI_MAX - 1, not NUL
     * terminated in-band); 0 for plain TCP and UDP. */
    uint16_t sni_len;
    uint8_t sni[NET_SOCKET_SNI_MAX];
} net_socket_open_descriptor_v1_t;

/* Interface-address record for NET_IPC_IFADDR_ADD/DEL/LIST. ADD carries one
 * record in a borrowed xfer buffer (arg0=buffer_id, arg1=borrow_id,
 * arg2=bytes); LIST fills an array of these into a client buffer and returns
 * the count in the reply arg0. All IPv4 words are network byte order. */
#define NET_IFADDR_RECORD_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t version;
    uint32_t if_index;
    uint32_t addr_v4;
    uint32_t netmask_v4;
    uint32_t gateway_v4;
    uint32_t flags; /* bit0: link up, bit1: administratively up */
} net_ifaddr_record_v1_t;

#define NET_IFADDR_FLAG_LINK_UP 1u
#define NET_IFADDR_FLAG_ADMIN_UP 2u
#define NET_IFADDR_FLAG_DHCP 4u /* address is (or is being) assigned by DHCP */

typedef struct {
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_drops;
    uint32_t tx_drops;
    uint32_t rx_errors;
    uint32_t tx_errors;
} netdrv_stats_t;

enum {
    WASMOS_DMA_DIR_TO_DEVICE = 1 << 0,
    WASMOS_DMA_DIR_FROM_DEVICE = 1 << 1,
    WASMOS_DMA_DIR_BIDIR = WASMOS_DMA_DIR_TO_DEVICE | WASMOS_DMA_DIR_FROM_DEVICE
};

/* DMA statuses are the packed dma domain in abi/errors.yaml:
 * WASMOS_ERR_NONE (0) on success, else a negative WASMOS_ERR_DMA_*. */

enum { WASMOS_DMA_SYNC_TO_DEVICE = 1, WASMOS_DMA_SYNC_FROM_DEVICE = 2, WASMOS_DMA_SYNC_BIDIR = 3 };

/* Cache policy for wasmos_region_alloc (driver-owned pinned DMA regions). */
enum {
    WASMOS_REGION_CACHE_WB = 0, /* write-back: coherent, virtqueue rings on x86 */
    WASMOS_REGION_CACHE_WC = 1  /* write-combining: framebuffer/scanout (TODO) */
};

/* Trigger/polarity flags for wasmos_irq_configure. Default (no flags) is
 * edge-triggered active-high (ISA). PCI INTx lines are level + active-low. */
enum { WASMOS_IRQ_TRIGGER_LEVEL = 1 << 0, WASMOS_IRQ_POLARITY_LOW = 1 << 1 };

/* --- Module metadata descriptor --------------------------------------------
 *
 * What process-manager reports about one packaged driver: how it matches, what
 * capabilities it asked for, and which register windows it declared. A
 * descriptor because the region list is variable-length by nature and the four
 * argument words of the packed form are already full -- there are spare bits in
 * them, but a region declaration is not a bit field and pretending otherwise is
 * how the device-publish encoding ended up smuggling an I/O base through an
 * unused argument half.
 *
 * The consumer supplies the buffer and lends it WRITE; PM fills it and replies
 * with the byte count. Versioned and length-checked, so it grows by appending. */
#define WASMOS_MODULE_META_DESC_VERSION 1u

/* Region kinds. Mirrors the app-format enum in src/kernel/include/wasmos_app.h;
 * a service cannot include kernel headers, so the two must move together. */
enum { WASMOS_APP_REGION_IO = 0, WASMOS_APP_REGION_BAR = 1 };

/* One declared register window: a fixed I/O range, or a BAR of the matched
 * device resolved by whoever holds the device record. */
typedef struct __attribute__((packed)) {
    uint8_t kind;
    uint8_t bar_index;
    uint16_t first;
    uint16_t last;
} wasmos_region_desc_t;

#define WASMOS_MODULE_META_MAX_REGIONS 4u

typedef struct __attribute__((packed)) {
    uint32_t version; /* = WASMOS_MODULE_META_DESC_VERSION */
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t storage_bootstrap;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t io_port_min; /* the match rule's window; regions supersede it */
    uint16_t io_port_max;
    uint32_t cap_flags;
    uint32_t match_count;
    uint32_t region_count;
    wasmos_region_desc_t regions[WASMOS_MODULE_META_MAX_REGIONS];
} wasmos_module_meta_desc_t;

/* --- PCI device descriptor -------------------------------------------------
 *
 * What pci-bus publishes about one enumerated function. A descriptor in a
 * transfer buffer rather than packed IPC arguments, because the interesting
 * facts about a PCI function do not fit in four words and keep growing: six
 * BARs, capability offsets, class triplet, interrupt routing. The packed form
 * had already been reduced to smuggling a second I/O base through the unused
 * half of an argument.
 *
 * Extensible the same way svc_register_desc_t is: bump the version, append
 * fields, and let the reader check the byte length it was given. */
#define WASMOS_PCI_DEVICE_DESC_VERSION 1u
#define WASMOS_PCI_BAR_COUNT 6u

enum {
    WASMOS_PCI_BAR_NONE = 0,  /* unimplemented: reads as zero */
    WASMOS_PCI_BAR_IO = 1,    /* I/O port window */
    WASMOS_PCI_BAR_MEM32 = 2, /* 32-bit memory window */
    WASMOS_PCI_BAR_MEM64 = 3  /* 64-bit memory window; consumes the next slot too */
};

/* One decoded BAR. `size` is 0 when unknown -- see the note in pci_bus.c on why
 * the size probe is not run during enumeration. */
typedef struct __attribute__((packed)) {
    uint8_t kind; /* WASMOS_PCI_BAR_* */
    uint8_t prefetchable;
    uint16_t reserved0;
    uint32_t reserved1;
    uint64_t base;
    uint64_t size;
} wasmos_pci_bar_t;

typedef struct __attribute__((packed)) {
    uint32_t version; /* = WASMOS_PCI_DEVICE_DESC_VERSION */
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t irq_line; /* config 0x3C; 0xFF = not routed */
    uint8_t irq_pin;  /* config 0x3D; 0 = does not assert INTx */
    uint16_t vendor_id;
    uint16_t device_id;
    /* Config-space offsets of the interrupt capabilities, 0 when absent. Only
     * pci-bus can walk the capability list, so it reports what it found rather
     * than making every consumer ask. */
    uint16_t msi_cap_offset;
    uint16_t msix_cap_offset;
    wasmos_pci_bar_t bars[WASMOS_PCI_BAR_COUNT];
} wasmos_pci_device_desc_t;

/* Whole sectors one BLOCK_IPC_READ_ZC_REQ may carry. A bound is needed because
 * the request names a single contiguous run; the server is free to transfer
 * fewer and reports the count it managed in BLOCK_IPC_READ_RESP.arg1, so a
 * server with a smaller limit stays correct without the client knowing. */
#define WASMOS_BLOCK_ZC_MAX_SECTORS 8u

/* BLOCK_IPC_READ_ZC_REQ.arg2 packs the borrow handle above the sector count, the
 * same (handle << 12 | small scalar) shape the spawn and mount paths already use.
 * A fifth field is needed because the server must be able to address the buffer
 * two ways -- by OBJECT to copy into it, by BORROW to map it for device DMA --
 * and dst_offset is the one field that can legitimately grow, so it keeps a slot
 * of its own rather than sharing one. The count needs 4 bits; 12 leaves room. */
#define WASMOS_BLOCK_ZC_BORROW_SHIFT 12u
#define WASMOS_BLOCK_ZC_COUNT_MASK 0xFFFu

/* Message-signalled interrupt style a PCI function supports, reported by
 * PCI_IPC_MSI_QUERY. MSI-X wins when a device offers both: it addresses each
 * vector independently, where plain MSI needs one naturally-aligned block of
 * consecutive vectors and so is limited to a single message here. */
enum { WASMOS_PCI_MSI_KIND_NONE = 0, WASMOS_PCI_MSI_KIND_MSI = 1, WASMOS_PCI_MSI_KIND_MSIX = 2 };

/* IPC message type the kernel sends for an MSI event (see src/kernel/include/msi.h).
 * arg0 is the table entry index the driver programmed, i.e. which of its own
 * interrupt sources fired. Unlike IPC_IRQ_EVENT_TYPE no ack is owed: the vector
 * is edge-triggered and exclusively owned, so nothing is masked waiting for one. */
#define WASMOS_IPC_MSI_EVENT_TYPE 0xFF01

typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
} wasmos_dma_window_t;

/* One inclusive I/O-port window [first, last]. A driver needs more than one
 * whenever its device's registers are not contiguous: legacy IDE is the case
 * that forced this — the task-file sits at the fixed ISA ports (0x1F0/0x3F6)
 * while its bus-master registers live in a firmware-assigned BAR (0xC040 on
 * QEMU's PIIX). Covering both with a single range would span every port in
 * between, which is where other devices live. */
typedef struct __attribute__((packed)) {
    uint16_t first;
    uint16_t last;
} wasmos_io_range_t;

/* Windows one spawn profile may carry. Small on purpose: a driver that needs
 * more than a handful of disjoint port windows is describing a device this
 * model does not fit. */
#define WASMOS_IO_RANGE_LIMIT 4u

typedef struct __attribute__((packed)) {
    uint32_t direction_flags;
    uint32_t max_bytes;
    uint32_t window_count;
    uint32_t reserved0;
} wasmos_spawn_dma_caps_t;

/* Descriptor-based spawn capabilities. Unlike the packed-arg spawn opcodes,
 * which are out of IPC argument slots, this carries variable-length arrays after
 * the fixed header. Layout is header, then io_range_count I/O windows, then
 * dma.window_count DMA windows — order matters, both sides walk it positionally.
 *
 * io_port_min/max remain for a single contiguous window; io_range_count > 0
 * supersedes them, which is how a device with disjoint register windows (legacy
 * IDE: task file at 0x1F0/0x3F6, bus-master registers in a BAR) is described. */
typedef struct __attribute__((packed)) {
    uint32_t cap_flags;
    uint16_t io_port_min;
    uint16_t io_port_max;
    uint16_t irq_mask;
    uint16_t io_range_count;
    wasmos_spawn_dma_caps_t dma;
    /* wasmos_io_range_t io_ranges[io_range_count];
     * wasmos_dma_window_t windows[dma.window_count]; */
} wasmos_spawn_caps_v2_t;

#define WASMOS_SPAWN_CAPS_V2_SIZE(io_range_count, window_count)                                    \
    (sizeof(wasmos_spawn_caps_v2_t) +                                                              \
     ((uint32_t)(io_range_count) * (uint32_t)sizeof(wasmos_io_range_t)) +                          \
     ((uint32_t)(window_count) * (uint32_t)sizeof(wasmos_dma_window_t)))

enum {
    WASMOS_IPC_FIELD_TYPE = 0,
    WASMOS_IPC_FIELD_REQUEST_ID = 1,
    WASMOS_IPC_FIELD_ARG0 = 2,
    WASMOS_IPC_FIELD_ARG1 = 3,
    WASMOS_IPC_FIELD_SOURCE = 4,
    WASMOS_IPC_FIELD_DESTINATION = 5,
    WASMOS_IPC_FIELD_ARG2 = 6,
    WASMOS_IPC_FIELD_ARG3 = 7
};

#endif
