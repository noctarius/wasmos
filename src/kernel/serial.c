/* serial.c - COM1 UART serial driver and kernel console output.
 * Drives COM1 (0x3F8) at 115200 baud for early and runtime debug output.
 * Every write also fans out to the console_ring_t shared-memory ring for
 * user-space readers, to the VT-owned klog ring when one is registered, and to a
 * small early-log capture buffer for driver handoff.
 * The state on the logging path (lock, console ring, klog ring, early log) is
 * reached through *_slot() accessors that add the kernel higher-half offset once
 * serial_enable_high_alias(1) is set, so logging works under a root with no low
 * identity mapping. The input ring below has no such accessor. */
#include <stdarg.h>
#include "console_ring.h"
#include "ipc.h"
#include "memory.h"
#include "serial.h"
#include "stdio.h"

#include "process.h"
#include "sync/spinlock.h"
#include "paging.h"
#include "xfer_buffer.h"
#include "wasmos/ringbuf.h"
/* SERIAL_DRIVER_* opcodes come from the generated IPC opcode ABI (abi/opcodes.yaml). */
#include "../../abi/generated/c/wasmos_opcodes.h"

#define COM1_PORT 0x3F8
#define COM1_STATUS (COM1_PORT + 5)

#define SERIAL_READ_STATUS_CHAR 1
#define SERIAL_READ_STATUS_EMPTY 0
#define SERIAL_READ_STATUS_ERROR (-1)
extern uint8_t __kernel_start;
extern uint8_t __kernel_end;

/* True when p is a low-VA pointer into the kernel image (a string literal
 * reached through the identity map) that must be rebased to the higher-half
 * alias.  __kernel_start/__kernel_end are linked high, so subtracting the base
 * gives the image's low window. */
static inline int serial_ptr_needs_kernel_alias(uintptr_t p) {
    if (!serial_high_alias_enabled() || p == 0) {
        return 0;
    }
    uint64_t base = KERNEL_HIGHER_HALF_BASE;
    if ((uint64_t)p >= base) {
        return 0;
    }
    uint64_t start = addr_cast(uint64_t, &__kernel_start);
    uint64_t end = addr_cast(uint64_t, &__kernel_end);
    uint64_t low_start = start - base;
    uint64_t low_end = end - base;
    return ((uint64_t)p >= low_start && (uint64_t)p < low_end) ? 1 : 0;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

#define EARLY_LOG_SIZE 4096
static uint8_t g_early_log[EARLY_LOG_SIZE];
static uint32_t g_early_log_head = 0;  /* next write index (wraps) */
static uint32_t g_early_log_count = 0; /* bytes written, capped at EARLY_LOG_SIZE */

static ksync_spinlock_t g_serial_lock = {0};
static uint8_t g_serial_high_alias_enabled = 0;
static uint32_t g_console_ring_shmem_id = 0;
static console_ring_t* g_console_ring = 0;

/* klog ring: physical base + byte size of the VT-owned SPSC ring.
 * phys == 0 means no VT ring is registered yet, so klog_ring_write is a no-op
 * and early boot keeps flowing only to console_ring + COM1 TX. */
static uint64_t g_klog_ring_phys = 0;
static uint32_t g_klog_ring_bytes = 0;

static inline ksync_spinlock_t* serial_lock_ptr(void) {
    uintptr_t addr = (uintptr_t)&g_serial_lock;
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (ksync_spinlock_t*)(void*)addr;
}

/* Arms the higher-half rebasing described in the file header.  Set it once the
 * kernel is executing from the higher-half alias and the low identity map may be
 * gone; every *_slot() accessor, the string-literal rebasing, and the console and
 * klog ring pointers key off this one flag.  Any non-zero value enables it.
 * Turning it back off while running from a root without a low identity map makes
 * the logging path dereference unmapped low addresses. */
void serial_enable_high_alias(uint8_t enabled) {
    g_serial_high_alias_enabled = enabled ? 1 : 0;
}

/* Normalised to 0 or 1.  Also read from libc.c's kernel_str_ptr, which is why it
 * has to stay cheap and side-effect free. */
uint8_t serial_high_alias_enabled(void) {
    return g_serial_high_alias_enabled;
}

static inline console_ring_t** serial_console_ring_slot(void) {
    uintptr_t addr = (uintptr_t)&g_console_ring;
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (console_ring_t**)(void*)addr;
}

static inline uint32_t* serial_console_ring_id_slot(void) {
    uintptr_t addr = (uintptr_t)&g_console_ring_shmem_id;
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (uint32_t*)(void*)addr;
}

static inline uint64_t* serial_klog_ring_phys_slot(void) {
    uintptr_t addr = (uintptr_t)&g_klog_ring_phys;
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (uint64_t*)(void*)addr;
}

static inline uint32_t* serial_klog_ring_bytes_slot(void) {
    uintptr_t addr = (uintptr_t)&g_klog_ring_bytes;
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (uint32_t*)(void*)addr;
}

static inline uint8_t* serial_early_log_buf(void) {
    uintptr_t addr = (uintptr_t)&g_early_log[0];
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (uint8_t*)(void*)addr;
}

static inline uint32_t* serial_early_log_head_slot(void) {
    uintptr_t addr = (uintptr_t)&g_early_log_head;
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (uint32_t*)(void*)addr;
}

static inline uint32_t* serial_early_log_count_slot(void) {
    uintptr_t addr = (uintptr_t)&g_early_log_count;
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (uint32_t*)(void*)addr;
}

static int serial_tx_ready(void) {
    return (inb(COM1_STATUS) & 0x20) != 0;
}

static int serial_rx_ready(void) {
    return (inb(COM1_STATUS) & 0x01) != 0;
}

static void com1_serial_init(void) {
    outb(COM1_PORT + 1, 0x00); // Disable interrupts
    outb(COM1_PORT + 3, 0x80); // Enable DLAB
    outb(COM1_PORT + 0, 0x01); // Divisor low (115200 baud)
    outb(COM1_PORT + 1, 0x00); // Divisor high
    outb(COM1_PORT + 3, 0x03); // 8 bits, no parity, one stop
    outb(COM1_PORT + 2, 0xC7); // Enable FIFO, clear, 14-byte threshold
    outb(COM1_PORT + 4, 0x0B); // IRQs enabled, RTS/DSR set
}

static void com1_serial_put_char(char c) {
    while (!serial_tx_ready()) {
    }
    outb(COM1_PORT, (uint8_t)c);
}

static int com1_serial_read_char(uint8_t* out_char) {
    if (!out_char) {
        return -1;
    }
    if (!serial_rx_ready()) {
        return 0;
    }
    *out_char = inb(COM1_PORT);
    return 1;
}

static const serial_driver_t g_com1_driver = {
    .init = com1_serial_init,
    .put_char = com1_serial_put_char,
    .read_char = com1_serial_read_char,
};

static const serial_driver_t* g_serial_driver = &g_com1_driver;

static uint32_t g_serial_remote_endpoint = IPC_ENDPOINT_NONE;
static uint32_t g_serial_remote_reply_endpoint = IPC_ENDPOINT_NONE;
static uint32_t g_serial_remote_next_request_id = 1;
static uint32_t g_serial_remote_pending_read_request = 0;

/* Keyboard input ring — filled by vt through serial_input_push and drained by
 * serial_input_read, which backs the wasmos_input_read host call.  Independent
 * of serial_read_char: a guest polling this ring sees only pushed keystrokes and
 * gets WASMOS_AGAIN when it is empty, with no COM1 fallback. */
#define INPUT_RING_SIZE 64
static uint8_t g_input_ring[INPUT_RING_SIZE];
static uint32_t g_input_head = 0;
static uint32_t g_input_count = 0;

static void serial_remote_reset(void) {
    g_serial_remote_endpoint = IPC_ENDPOINT_NONE;
    g_serial_remote_pending_read_request = 0;
    g_serial_remote_next_request_id = 1;
}

static void serial_ring_init(void) {
    console_ring_t** ring_slot = serial_console_ring_slot();
    uint32_t* ring_id_slot = serial_console_ring_id_slot();
    if (*ring_slot) {
        return;
    }
    uint64_t phys_base = 0;
    if (mm_shared_create(0, 1, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, ring_id_slot,
                         &phys_base) != 0) {
        return;
    }
    if (mm_shared_retain(0, *ring_id_slot) != 0) {
        *ring_id_slot = 0;
        return;
    }
    *ring_slot = ptr_cast(console_ring_t, phys_base);
    (*ring_slot)->write_pos = 0;
    (*ring_slot)->read_pos = 0;
    (*ring_slot)->capacity = CONSOLE_RING_DATA_SIZE;
    (*ring_slot)->_pad = 0;
}

static uint32_t serial_remote_next_request_id(void) {
    uint32_t value = g_serial_remote_next_request_id++;
    if (g_serial_remote_next_request_id == 0) {
        g_serial_remote_next_request_id = 1;
    }
    return value;
}

static int serial_remote_send_message(uint32_t type, uint32_t request_id, uint32_t arg0,
                                      uint32_t arg1) {
    if (g_serial_remote_endpoint == IPC_ENDPOINT_NONE ||
        g_serial_remote_reply_endpoint == IPC_ENDPOINT_NONE) {
        return IPC_ERR_INVALID;
    }

    ipc_message_t req = {
        .type = type,
        .source = g_serial_remote_reply_endpoint,
        .destination = g_serial_remote_endpoint,
        .request_id = request_id,
        .arg0 = arg0,
        .arg1 = arg1,
        .arg2 = 0,
        .arg3 = 0,
    };

    int rc = ipc_send_from(IPC_CONTEXT_KERNEL, g_serial_remote_endpoint, &req);
    if (rc != IPC_OK) {
        /* Drop the link only when the endpoint itself is unusable. A full
         * queue is transient and must NOT reset -- that is why these are
         * distinct codes rather than one INVALID. */
        if (rc == IPC_ERR_NOENT || rc == IPC_ERR_PEER_GONE || rc == IPC_ERR_UNSUPPORTED ||
            rc == IPC_ERR_PERM) {
            serial_remote_reset();
        }
    }
    return rc;
}

static int serial_remote_read_char(uint8_t* out_char) {
    if (!out_char || g_serial_remote_endpoint == IPC_ENDPOINT_NONE ||
        g_serial_remote_reply_endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }

    if (g_serial_remote_pending_read_request != 0) {
        ipc_message_t resp;
        int rc = ipc_recv_for(IPC_CONTEXT_KERNEL, g_serial_remote_reply_endpoint, &resp);
        if (rc == IPC_OK) {
            if (resp.type != SERIAL_DRIVER_RESP ||
                resp.request_id != g_serial_remote_pending_read_request) {
                serial_remote_reset();
                return -1;
            }
            g_serial_remote_pending_read_request = 0;
            int32_t status = (int32_t)resp.arg1;
            if (status == SERIAL_READ_STATUS_CHAR) {
                *out_char = (uint8_t)resp.arg0;
                return 1;
            }
            if (status == SERIAL_READ_STATUS_EMPTY) {
                return 0;
            }
            return -1;
        }
        if (rc == IPC_EMPTY) {
            return 0;
        }
        serial_remote_reset();
        return -1;
    }

    uint32_t request_id = serial_remote_next_request_id();
    if (serial_remote_send_message(SERIAL_DRIVER_READ_REQ, request_id, 0, 0) != IPC_OK) {
        return -1;
    }
    g_serial_remote_pending_read_request = request_id;
    return 0;
}

/* Appends one byte to the 64-entry keyboard ring under g_serial_lock.  A full
 * ring drops the byte silently: keystrokes are advisory and blocking here would
 * stall whatever driver context pushed it.
 *
 * Because it takes the same lock as serial_write, this must not run from a
 * context that can interrupt a locked writer on the same CPU. */
void serial_input_push(uint8_t ch) {
    ksync_spinlock_lock(serial_lock_ptr());
    if (g_input_count < INPUT_RING_SIZE) {
        uint32_t idx = (g_input_head + g_input_count) % INPUT_RING_SIZE;
        g_input_ring[idx] = ch;
        g_input_count++;
    }
    ksync_spinlock_unlock(serial_lock_ptr());
}

/* Pops the oldest pushed keystroke.  Returns 1 and stores it in *out, or 0 with
 * *out untouched when the ring is empty — never blocks, and never falls back to
 * COM1.  out is not checked for NULL and is dereferenced whenever a byte is
 * available.  Takes g_serial_lock. */
int serial_input_read(uint8_t* out) {
    ksync_spinlock_lock(serial_lock_ptr());
    if (g_input_count == 0) {
        ksync_spinlock_unlock(serial_lock_ptr());
        return 0;
    }
    *out = g_input_ring[g_input_head];
    g_input_head = (g_input_head + 1) % INPUT_RING_SIZE;
    g_input_count--;
    ksync_spinlock_unlock(serial_lock_ptr());
    return 1;
}

/* Shared-memory id of the one-page console ring, creating it on first use.
 * Returns 0 when creation fails, which is the same value as "not yet created" —
 * the caller cannot distinguish the two, and a later call retries. */
uint32_t serial_console_ring_id(void) {
    uint32_t* ring_id_slot = serial_console_ring_id_slot();
    if (*ring_id_slot == 0) {
        serial_ring_init();
    }
    return *ring_id_slot;
}

/* The console ring as the raw value mm_shared_create handed back: a PHYSICAL
 * base, NOT rebased onto the higher-half alias even when that alias is armed.
 * serial_ring_write does that rebasing itself on every write; a caller that
 * dereferences this pointer has to do the same, or run under a root that still
 * identity-maps low memory.  0 when the ring could not be created. */
void* serial_console_ring_ptr(void) {
    console_ring_t** ring_slot = serial_console_ring_slot();
    if (!*ring_slot) {
        serial_ring_init();
    }
    return *ring_slot;
}

/* Points the console's READ path at a user-space serial driver reachable at
 * `endpoint`, creating the kernel-owned reply endpoint on first use and clearing
 * any in-flight read request.  The transmit path is unaffected and keeps writing
 * COM1 directly (see serial_transmit).
 *
 * Returns 0 on success and -1 when the endpoint is IPC_ENDPOINT_NONE, has no
 * owner, is owned by the kernel itself (which would make the kernel its own
 * peer), or when the reply endpoint cannot be created.  The link is dropped
 * again automatically by serial_remote_reset on a fatal IPC error. */
int serial_register_remote_driver(uint32_t endpoint) {
    if (endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }

    uint32_t owner = 0;
    if (ipc_endpoint_owner(endpoint, &owner) != IPC_OK) {
        return -1;
    }

    if (owner == IPC_CONTEXT_KERNEL) {
        return -1;
    }

    if (g_serial_remote_reply_endpoint == IPC_ENDPOINT_NONE) {
        if (ipc_endpoint_create(IPC_CONTEXT_KERNEL, &g_serial_remote_reply_endpoint) != IPC_OK) {
            return -1;
        }
    }

    serial_remote_reset();
    g_serial_remote_endpoint = endpoint;
    return 0;
}

/* Installs a driver vtable and returns the previous one, so a caller can restore
 * it.  A NULL argument restores the built-in COM1 driver rather than clearing
 * the slot.  The vtable is borrowed, not copied: it must outlive the
 * installation.
 *
 * Only the init hook is reached from the kernel's own paths.  serial_transmit
 * and serial_read_char call COM1 directly and deliberately bypass this pointer,
 * because both run in contexts where a low-address global must not be
 * dereferenced. */
const serial_driver_t* serial_set_driver(const serial_driver_t* driver) {
    const serial_driver_t* prev = g_serial_driver;
    g_serial_driver = driver ? driver : &g_com1_driver;
    return prev;
}

/* Never NULL: the slot is initialised to the built-in COM1 driver and
 * serial_set_driver refuses to clear it. */
const serial_driver_t* serial_get_driver(void) {
    return g_serial_driver;
}

static void serial_put_internal(char c) {
    /* Ring-3 strict mode must not depend on low-address global driver state.
     * Keep TX path CR3-invariant by using direct COM1 I/O. */
    com1_serial_put_char(c);
}

static void serial_ring_write(const char* s) {
    console_ring_t* ring = *serial_console_ring_slot();
    if (!ring || !s) {
        return;
    }
    /* After the higher-half alias switch the ring holds a raw physical pointer.
     * Convert it to the kernel higher-half alias so writes reach mapped memory
     * from any CR3.  The ring is allocated well below 512 MiB so the alias
     * always falls inside the shared higher-half window. */
    if (g_serial_high_alias_enabled && addr_cast(uint64_t, ring) < KERNEL_HIGHER_HALF_BASE) {
        ring = ptr_cast(console_ring_t, (addr_cast(uint64_t, ring) + KERNEL_HIGHER_HALF_BASE));
    }
    uint32_t cap = ring->capacity;
    uint32_t wp = ring->write_pos;
    if (cap == 0) {
        return;
    }
    while (*s) {
        ring->data[wp % cap] = (uint8_t)*s++;
        wp++;
    }
    ring->write_pos = wp;
}

/* Publish a klog string into the VT-owned SPSC ring, if one is registered.
 * Additive to serial_ring_write; a short write (ring near full) silently drops
 * the tail — klog is advisory and COM1 TX carries the full log regardless. */
static void klog_ring_write(const char* s) {
    uint64_t phys = *serial_klog_ring_phys_slot();
    if (!phys || !s) {
        return;
    }
    uint32_t bytes = *serial_klog_ring_bytes_slot();
    /* The stored base is a raw physical address; reach it through the kernel
     * higher-half alias so writes land from any CR3, exactly as
     * serial_ring_write does for console_ring. */
    if (g_serial_high_alias_enabled && phys < KERNEL_HIGHER_HALF_BASE) {
        phys += KERNEL_HIGHER_HALF_BASE;
    }
    wasmos_ringbuf_t rb;
    if (wasmos_ringbuf_attach(&rb, ptr_cast(void, phys), bytes) != 0) {
        return;
    }
    uint32_t len = 0;
    while (s[len]) {
        len++;
    }
    if (len) {
        (void)wasmos_ringbuf_write(&rb, s, len);
    }
}

/* Adopts an already-initialised ringbuf, owned by owner_context_id as
 * xfer-buffer `id`, as the destination of the additional klog stream.  Only one
 * ring is tracked; a second successful call replaces the first, and there is no
 * unregister.
 *
 * Returns 0 once the ring is live, -1 for a zero id, an id that does not resolve
 * to a BUFFER_KIND_TRANSFER buffer of that owner, a base that is zero or not
 * page-aligned, a zero size, or a region that does not carry a valid ringbuf
 * header.  The rejection matters: an accepted bad region would corrupt memory on
 * every subsequent klog write. */
int klog_register_ring(uint32_t owner_context_id, uint32_t id) {
    if (id == 0) {
        return -1;
    }
    /* The VT owns the ring as a BUFFER_KIND_TRANSFER xfer-buffer (the same
     * zero-copy transport the socket rings use); resolve it by (id, owner) and
     * take its physical base.  No pin/retain: the VT holds the buffer for its
     * whole lifetime, matching the kernel's fixed-region console_ring model. */
    xfer_buffer_t buf;
    if (xfer_buffer_describe(id, BUFFER_KIND_TRANSFER, owner_context_id, &buf) != WASMOS_ERR_NONE) {
        return -1;
    }
    uint64_t base = xfer_buffer_object_phys(&buf);
    uint32_t region_bytes = buf.size_bytes;
    if (base == 0 || (base & 0xFFFull) != 0 || region_bytes == 0) {
        return -1;
    }
    uint64_t alias = base;
    if (g_serial_high_alias_enabled && alias < KERNEL_HIGHER_HALF_BASE) {
        alias += KERNEL_HIGHER_HALF_BASE;
    }
    /* Validate the region is an initialized ringbuf before accepting it, so a
     * bad/foreign id cannot wedge klog output. */
    wasmos_ringbuf_t rb;
    if (wasmos_ringbuf_attach(&rb, ptr_cast(void, alias), region_bytes) != 0) {
        return -1;
    }
    *serial_klog_ring_bytes_slot() = region_bytes;
    /* Publish phys last: klog_ring_write reads phys first, so a non-zero phys
     * always implies a valid bytes value is already visible. */
    *serial_klog_ring_phys_slot() = base;
    return 0;
}

static void serial_transmit(char c) {
    /* Always use the direct driver for output.  Per-character IPC to the
     * remote serial service overflows the depth-32 endpoint queue for any
     * string longer than 32 bytes; chars beyond slot 32 fall back to direct
     * COM1 while the earlier chars are still queued, inverting their on-wire
     * order.  The remote endpoint is retained for read requests only.
     * The framebuffer driver consumes a shared-memory console ring instead of
     * per-character IPC forwarding from serial_write. */
    serial_put_internal(c);
}

/* Programs the UART through the installed driver's init hook and creates the
 * console ring.  Safe to call again: com1_serial_init only rewrites UART
 * registers, and serial_ring_init returns immediately once a ring exists.
 * Failure to create the ring is not reported; logging then reaches COM1 and the
 * early-log buffer only. */
void serial_init(void) {
    if (g_serial_driver && g_serial_driver->init) {
        g_serial_driver->init();
    }
    serial_ring_init();
}

/* The normal console writer: takes g_serial_lock so concurrent CPUs cannot
 * interleave their output, then does the work in serial_write_unlocked.
 *
 * Spins on the UART's TX-ready bit for every byte, so cost is proportional to
 * the string at 115200 baud.  NULL is ignored.  Not safe from an exception, NMI
 * or panic path, or from anything that can interrupt a CPU already inside this
 * lock — use serial_write_unlocked there. */
void serial_write(const char* s) {
    if (!s) {
        return;
    }
    ksync_spinlock_lock(serial_lock_ptr());
    serial_write_unlocked(s);
    ksync_spinlock_unlock(serial_lock_ptr());
}

/* Formats through vsnprintf into a 512-byte stack buffer and emits it with
 * serial_write, so it takes g_serial_lock and carries the same context
 * restrictions.  Output longer than 511 bytes is truncated, silently.  fmt is
 * rebased onto the higher-half alias when it points into the kernel image; the
 * variadic string arguments are rebased later, inside vsnprintf. */
void serial_printf(const char* fmt, ...) {
    char buf[512];
    if (serial_ptr_needs_kernel_alias((uintptr_t)fmt)) {
        fmt = ptr_cast(char, (addr_cast(uint64_t, fmt) + KERNEL_HIGHER_HALF_BASE));
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_write(buf);
}

/* serial_printf without the lock, for panic, exception and NMI paths.  See
 * serial_write_unlocked for why the unlocked variants exist and what they give
 * up. */
void serial_printf_unlocked(const char* fmt, ...) {
    char buf[512];
    if (serial_ptr_needs_kernel_alias((uintptr_t)fmt)) {
        fmt = ptr_cast(char, (addr_cast(uint64_t, fmt) + KERNEL_HIGHER_HALF_BASE));
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_write_unlocked(buf);
}

/* Emits value as a fixed 16-digit uppercase "0x…" line with a trailing newline —
 * no width suppression, no vsnprintf, nothing that could allocate or recurse.
 * Takes g_serial_lock. */
void serial_write_hex64(uint64_t value) {
    char buf[20];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\n';
    buf[19] = '\0';
    serial_write(buf);
}

/* serial_write_hex64 without the lock.  The lowest-dependency way to get a
 * 64-bit value out of a fault handler: stack buffer only, no formatter, no
 * lock. */
void serial_write_hex64_unlocked(uint64_t value) {
    char buf[20];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\n';
    buf[19] = '\0';
    serial_write_unlocked(buf);
}

/* The whole write path — console ring, klog ring, COM1 TX with LF->CRLF
 * expansion, early-log capture — with the lock deliberately NOT taken.
 *
 * It exists for contexts that cannot wait on g_serial_lock: a panic or exception
 * handler may have interrupted a CPU that is holding it, and an NMI can arrive
 * while this very CPU holds it, so taking it there deadlocks instead of
 * printing.  The cost is that output from another CPU can interleave
 * mid-string, and a concurrent writer can corrupt the ring positions; a
 * garbled panic line is the accepted trade for one that appears at all.
 *
 * Also the body of serial_write, which supplies the lock.  Preemption is
 * disabled across the transmit loop either way, so a preemptible caller cannot
 * be descheduled part-way through a line.  A NULL string is ignored, and a
 * string literal in the kernel image is rebased onto the higher-half alias when
 * that alias is armed. */
void serial_write_unlocked(const char* s) {
    if (!s) {
        return;
    }
    if (g_serial_high_alias_enabled) {
        uintptr_t sp = (uintptr_t)s;
        if (serial_ptr_needs_kernel_alias(sp)) {
            s = ptr_cast(char, ((uint64_t)sp + KERNEL_HIGHER_HALF_BASE));
        }
    }
    if (!*serial_console_ring_slot()) {
        serial_ring_init();
    }
    uint8_t* early_log = serial_early_log_buf();
    uint32_t* early_head = serial_early_log_head_slot();
    uint32_t* early_count = serial_early_log_count_slot();
    preempt_disable();
    serial_ring_write(s);
    klog_ring_write(s);
    for (const char* p = s; *p; ++p) {
        if (*p == '\n') {
            serial_transmit('\r');
            early_log[*early_head] = '\r';
            *early_head = (*early_head + 1) % EARLY_LOG_SIZE;
            if (*early_count < EARLY_LOG_SIZE) {
                (*early_count)++;
            }
        }
        serial_transmit(*p);
        early_log[*early_head] = (uint8_t)*p;
        *early_head = (*early_head + 1) % EARLY_LOG_SIZE;
        if (*early_count < EARLY_LOG_SIZE) {
            (*early_count)++;
        }
    }
    preempt_enable();
}

/* Bytes currently retrievable through serial_early_log_copy, saturating at
 * EARLY_LOG_SIZE (4096) once the capture buffer has wrapped.  Read without the
 * lock, so it can grow between this call and the copy. */
uint32_t serial_early_log_size(void) {
    return *serial_early_log_count_slot();
}

/* Copies up to len bytes of the captured early log into dst, where offset 0 is
 * the OLDEST retained byte rather than a buffer index — the ring's wrap is
 * resolved here, so a caller can page through it with a plain counter.
 *
 * len is clamped to what remains after offset; an offset at or past the end
 * copies nothing.  There is no return value and no way to tell a clamp from a
 * full copy, so pair it with serial_early_log_size.  dst is a caller buffer of
 * at least len bytes, borrowed for the call; NULL is ignored.  Takes no lock, so
 * a concurrent serial_write can tear the result. */
void serial_early_log_copy(uint8_t* dst, uint32_t offset, uint32_t len) {
    uint8_t* early_log = serial_early_log_buf();
    uint32_t early_head = *serial_early_log_head_slot();
    uint32_t early_count = *serial_early_log_count_slot();
    if (!dst || offset >= early_count) {
        return;
    }
    if (len > early_count - offset) {
        len = early_count - offset;
    }
    /* Logical index 0 is the oldest byte. */
    uint32_t start =
        (early_count < EARLY_LOG_SIZE) ? 0 : early_head; /* head = oldest when ring is full */
    for (uint32_t i = 0; i < len; ++i) {
        dst[i] = early_log[(start + offset + i) % EARLY_LOG_SIZE];
    }
}

/* Non-blocking single-character read from the console.  Returns 1 with *out_char
 * set, 0 when no character is available yet, or -1 for a NULL out_char.
 *
 * A registered remote driver is tried first and is pipelined, not synchronous:
 * the first call with nothing outstanding sends a read request and returns 0, and
 * a later call collects the reply.  Any failure of that exchange falls through to
 * a direct COM1 poll, so the console keeps working when the driver dies.  Takes
 * no lock. */
int serial_read_char(uint8_t* out_char) {
    if (!out_char) {
        return -1;
    }
    int rc = serial_remote_read_char(out_char);
    if (rc >= 0) {
        return rc;
    }
    /* Match TX strictness: avoid low-address global driver deref here. */
    return com1_serial_read_char(out_char);
}
