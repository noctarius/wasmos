/* capability.h - Per-context hardware access capability checks.
 *
 * A capability record is attached to each process context by the process manager
 * when a driver is spawned.  The PM reads allowed resources from linker.metadata
 * in the WASMOS-APP package and calls capability_set_spawn_profile().
 * All hardware hostcalls (I/O port, IRQ, MMIO, DMA) then call the corresponding
 * capability_*_allowed() function before touching hardware. */
#ifndef WASMOS_CAPABILITY_H
#define WASMOS_CAPABILITY_H

#include <stdint.h>
#include "wasmos_driver_abi.h"

/* Sizes the fixed per-context arrays a spawn profile is copied into. A profile
 * declaring more windows than either limit is REFUSED, not truncated: silently
 * dropping the tail would leave the driver's region indices disagreeing with
 * the kernel's, so every window past the limit would fail far from the spawn
 * that over-declared it. PM_DMA_WINDOW_LIMIT must not exceed the DMA one. */
#define CAPABILITY_DMA_WINDOW_LIMIT 16u             /* max DMA windows per driver context */
#define CAPABILITY_REGION_LIMIT WASMOS_REGION_LIMIT /* max register windows */

/* Hardware access kinds, each gated by a separate flag in the spawn profile. */
typedef enum {
    CAP_IO_PORT = 0,            /* x86 I/O port in/out instructions */
    CAP_IRQ_ROUTE = 1,          /* register and receive IRQ events via IPC */
    CAP_MMIO_MAP = 2,           /* map MMIO regions into the process address space */
    CAP_DMA_BUFFER = 3,         /* allocate and share DMA-coherent buffers */
    CAP_SYSTEM_CONTROL = 4,     /* privileged kernel control (reboot, power off, etc.) */
    CAP_SUBSYSTEM_REGISTER = 5, /* register broker subsystems + executable-format handlers */
    CAP_SVC_CLASS_REGISTER = 6, /* register a service under a virtual class (anti-spoof) */
    CAP_CONSOLE_OWNER = 7,      /* register the console ring: all tty output and COM1 */
    CAP_SPAWN_GRANT = 8,        /* put hardware grants (a spawn profile) on a spawned child */
    CAP_SVC_CONTROL = 9,        /* submit, kill and declare the boot set at the supervisor */
    CAP_PCI_CONFIG = 10,        /* read and write PCI configuration space */
    CAP_RTC_EFI = 11,           /* read and set the clock through UEFI runtime services */
    CAP_INVENTORY_DEVICE = 12,  /* publish PCI/ACPI device records to device-manager */
    CAP_INVENTORY_STORAGE = 13, /* publish/retire block and volume records to device-manager */
    CAP_KEYRING_REGISTER = 14,  /* register a trusted package-signing key (the keyring) */
    CAP_KEYRING_ADMIN = 15      /* change what the keyring trusts (trust, the console shell) */
} capability_kind_t;

/* Kinds are numbered 0..CAPABILITY_KIND_COUNT-1. Their values are ABI: a guest
 * names one to ipc_endpoint_has_cap as WASMOS_CAP_* (abi/constants.yaml). */
#define CAPABILITY_KIND_COUNT 16u
_Static_assert(CAP_IO_PORT == WASMOS_CAP_IO_PORT && CAP_IRQ_ROUTE == WASMOS_CAP_IRQ_ROUTE &&
                   CAP_MMIO_MAP == WASMOS_CAP_MMIO_MAP && CAP_DMA_BUFFER == WASMOS_CAP_DMA_BUFFER &&
                   CAP_SYSTEM_CONTROL == WASMOS_CAP_SYSTEM_CONTROL &&
                   CAP_SUBSYSTEM_REGISTER == WASMOS_CAP_SUBSYSTEM_REGISTER &&
                   CAP_SVC_CLASS_REGISTER == WASMOS_CAP_SVC_CLASS_REGISTER &&
                   CAP_CONSOLE_OWNER == WASMOS_CAP_CONSOLE_OWNER &&
                   CAP_SPAWN_GRANT == WASMOS_CAP_SPAWN_GRANT &&
                   CAP_SVC_CONTROL == WASMOS_CAP_SVC_CONTROL &&
                   CAP_PCI_CONFIG == WASMOS_CAP_PCI_CONFIG && CAP_RTC_EFI == WASMOS_CAP_RTC_EFI &&
                   CAP_INVENTORY_DEVICE == WASMOS_CAP_INVENTORY_DEVICE &&
                   CAP_INVENTORY_STORAGE == WASMOS_CAP_INVENTORY_STORAGE &&
                   CAP_KEYRING_REGISTER == WASMOS_CAP_KEYRING_REGISTER &&
                   CAP_KEYRING_ADMIN == WASMOS_CAP_KEYRING_ADMIN,
               "capability_kind_t is the WASMOS_CAP_* ABI");

/* Initialize the capability table; called once during kernel startup.
 * Also seeds the record for context 0 (the kernel) with every capability set. */
void capability_init(void);

/* Maps a manifest capability name (an unterminated byte string) to its kind:
 * "io.port", "irq.route", "mmio.map", "dma.buffer", "system.control",
 * "subsystem.register", "svc.class", "console.owner", "spawn.grant",
 * "svc.control", "pci.config", "rtc.efi". The match is exact. Returns 0 with *out_kind set, or -1
 * for a NULL or empty name or an unrecognised one. */
int capability_kind_from_name(const uint8_t* name, uint32_t name_len, capability_kind_t* out_kind);

/* Grant the named-endpoint capability to context_id (used for IPC access control).
 * `name`/`name_len` is a manifest capability name (capability_kind_from_name);
 * anything else is refused. Creates the context's record if
 * absent, and ORs the bit in, so grants accumulate. `flags` is used only by
 * "dma.buffer", where it is the pinning budget IN PAGES and additionally
 * installs the platform-wide low-2-GiB DMA window (a spawn profile is what
 * narrows that). Returns 0 on success, -1 for a NULL/empty or unrecognised
 * name, or when the record cannot be allocated. */
int capability_grant_name(uint32_t context_id, const uint8_t* name, uint32_t name_len,
                          uint32_t flags);

/* Return non-zero if context_id holds the given hardware capability kind.
 * Holding a kind is necessary but not sufficient for access: the per-action
 * predicates below additionally require a spawn profile and check the
 * allowlist. Returns 0 for an unknown context. */
int capability_has(uint32_t context_id, capability_kind_t kind);

/* Every capability the context holds, one bit per kind (bit k for kind k); 0 for
 * a context with no record.  The kernel context (0) holds every bit.  A snapshot:
 * grants only accumulate, so it can only have grown since. */
uint32_t capability_mask_of(uint32_t context_id);

/* Whether a package's manifest may be granted `kind`, which is the image policy
 * of docs/architecture/10-capability-and-policy.md ("Signed Packages"):
 *   - the package must carry a valid signature by the build's key, or any
 *     process could write an image declaring the capability and spawn it;
 *   - for a kind that acts on requests (system.control, subsystem.register,
 *     svc.class, console.owner, spawn.grant, svc.control, pci.config, rtc.efi,
 *     inventory.device, inventory.storage), the process that requested the
 *     spawn must hold `kind` itself, or hold spawn.grant, whose holders
 *     (device-manager, the supervisor) spawn what others need; otherwise a
 *     process could spawn a privileged image as a deputy and steer it with
 *     arguments.  The hardware kinds (io.port, mmio.map, irq.route,
 *     dma.buffer) reach only what the signed manifest or a spawn profile --
 *     which only a spawn.grant holder can attach -- bounds, so the signature
 *     is their whole test.
 * requester_mask is capability_mask_of the requester when it asked (every bit for
 * a spawn the kernel made).  ipc.basic is not a kind and is not asked about. */
typedef enum {
    CAPABILITY_GRANT_OK = 0,
    CAPABILITY_GRANT_UNKNOWN = 1,     /* not a capability kind */
    CAPABILITY_GRANT_UNSIGNED = 2,    /* the package is not signed with the build's key */
    CAPABILITY_GRANT_REQUESTER = 3,   /* the requester holds neither the kind nor spawn.grant */
    CAPABILITY_GRANT_SYSTEM_ONLY = 4, /* a critical kind, and the image is a publisher's */
} capability_grant_check_t;

/* How far the package asking for a capability is trusted (image_trust): signed by
 * no trusted key, by the build's system key, or by a publisher's key (the build's
 * app key, or one the keyring registered). */
#define CAPABILITY_IMAGE_UNTRUSTED 0
#define CAPABILITY_IMAGE_SYSTEM 1
#define CAPABILITY_IMAGE_PUBLISHER 2
int capability_manifest_grant_check(capability_kind_t kind, int image_trust,
                                    uint32_t requester_mask);

/* Drops context_id's record -- grants and spawn profile -- when its process is
 * reaped, so records do not outlive their processes and every later lookup
 * scans only live contexts. A context without a record is a no-op. */
void capability_release_context(uint32_t context_id);

/* Return non-zero if a capability record exists for context_id. */
int capability_context_configured(uint32_t context_id);

/* One register window of a spawn profile: a wasmos_region_grant_t plus, for a
 * memory-mapped window, the kernel VA it is mapped at (device_window_map). The
 * process manager maps the window before installing the profile, so a profile
 * never holds an MMIO region without a mapping. kva is 0 for an I/O window. */
typedef struct {
    uint32_t kind;   /* WASMOS_REGION_GRANT_* */
    uint32_t length; /* ports (IO) or bytes (MMIO), never 0 */
    uint64_t base;   /* first port (IO) or physical address (MMIO) */
    uint64_t kva;    /* MMIO: kernel VA of `base`; IO: 0 */
} capability_region_t;

/* Set the full hardware access profile for a newly spawned driver process.
 * cap_flags is a DEVMGR_CAP_* bitmask. regions lists the register windows in
 * region-index order (a device's registers are not always contiguous, nor all
 * of one kind); irqs lists the interrupt identifiers the driver may route
 * (wasmos_spawn_caps_v2_t). dma_windows defines physical
 * address ranges the driver may use for DMA.
 *
 * The DMA group is applied only if its cap_flags bit is set, so clearing the
 * bit means "declare nothing", NOT "revoke": the DMA fields in
 * particular keep whatever a dma.buffer manifest grant left there. Every region
 * needs the bit of its kind (DEVMGR_CAP_IO_PORT, DEVMGR_CAP_MMIO_MAP) and is
 * refused, not dropped, without it. Returns 0, or -1 for a context whose
 * record cannot be created, more windows than the limits above, a region of
 * unknown kind, without its bit, of zero length, past port 0xFFFF or (MMIO)
 * without a mapping, interrupts without DEVMGR_CAP_IRQ or past
 * WASMOS_IRQ_GRANT_LIMIT, or a DMA bit set with no windows. Called once per driver
 * at spawn, before the child first runs. */
int capability_set_spawn_profile(uint32_t context_id, uint32_t cap_flags, uint32_t region_count,
                                 const capability_region_t* regions, uint32_t irq_count,
                                 const uint32_t* irqs, uint32_t dma_direction_flags,
                                 uint32_t dma_max_bytes, uint32_t dma_window_count,
                                 const wasmos_dma_window_t* dma_windows);

/* Return non-zero if a spawn profile (hardware limits) has been set for context_id. */
int capability_spawn_profile_configured(uint32_t context_id);

/* Predicate checks called by hardware hostcalls before granting access.
 *
 * All of these return NON-ZERO FOR ALLOWED and 0 for denied -- the opposite
 * polarity to the 0-on-success functions above, so do not mix them up. All deny
 * by default: an unknown context, or one without a spawn profile, is refused,
 * and so is a zero-length or zero-direction request. capability_region_resolve
 * is the exception and returns a packed error code; it is documented
 * separately. */
int capability_io_port_allowed(uint32_t context_id, uint16_t port);

/* Where one region access lands: an absolute I/O port, or the kernel VA of a
 * memory-mapped register. */
typedef struct {
    uint32_t kind;  /* WASMOS_REGION_GRANT_* */
    uint64_t where; /* IO: port (<= 0xFFFF); MMIO: kernel VA */
} capability_region_access_t;

/* Resolve (region, offset) for a context, where `region` is an index into the
 * windows the spawn profile granted, in the order they were declared, and all
 * `access_width` bytes (1, 2 or 4) must lie inside that window. An access to a
 * memory-mapped window must also be naturally aligned. Returns 0 and fills *out
 * on success, else a negative WASMOS_ERR_IO_* code naming which check refused
 * it -- "no such region", "offset past the end" and "misaligned" are different
 * bugs and a caller should be able to tell them apart.
 *
 * This is what lets a driver address its device without ever naming an absolute
 * port or address: it cannot express an access outside the window it was
 * granted, because it does not supply the base. */
int capability_region_resolve(uint32_t context_id, uint32_t region, uint32_t offset,
                              uint32_t access_width, capability_region_access_t* out);
/* Whether context_id's spawn profile lists interrupt identifier irq_line. The
 * architecture's irq_register still checks the identifier is one it routes. */
int capability_irq_line_allowed(uint32_t context_id, uint32_t irq_line);
/* Whether the context may map MMIO at all. There is no per-range MMIO
 * allowlist, so this is the only gate on which physical ranges it can reach. */
int capability_mmio_allowed(uint32_t context_id);
/* Every bit of direction_flags must be granted, not just one. */
int capability_dma_direction_allowed(uint32_t context_id, uint32_t direction_flags);
/* [base, base+length) must lie ENTIRELY within a single granted window;
 * a range spanning two adjacent windows is denied. Overflowing ranges too. */
int capability_dma_range_allowed(uint32_t context_id, uint64_t base, uint64_t length);
/* Declared pinning budget in bytes, or 0 when none was declared -- which the
 * budget check treats as "no driver-owned DMA regions at all". */
uint32_t capability_dma_max_bytes(uint32_t context_id);

/* Per-context DMA budget for driver-owned pinned regions (region_alloc). The
 * budget (dma_max_bytes) is declared by the driver's dma.buffer manifest
 * capability. capability_dma_charge tests and charges in one step under the
 * table lock: it returns 1 and adds `bytes` to the pinned total when that stays
 * within the budget, and 0, charging nothing, otherwise -- so reservations
 * outstanding on several CPUs together never exceed the budget. A zero `bytes`,
 * a missing profile or capability, and a zero budget are refused.
 * capability_dma_refund takes back a charge whose allocation was abandoned
 * (never below zero). Region allocations stay pinned for the context's
 * lifetime and are released with the context at reap, so a successful charge
 * is never refunded. */
int capability_dma_charge(uint32_t context_id, uint64_t bytes);
void capability_dma_refund(uint32_t context_id, uint64_t bytes);

#endif
