/* serial.c - COM1 UART serial driver and kernel console output.
 * Drives COM1 (0x3F8) at 115200 baud for early and runtime debug output.
 * Every write also fans out to the console_ring_t shared-memory ring for
 * user-space readers, and kernel log is appended to the kernel log ring (kmsg.h)
 * that dmesg, the boot replays and the kernel-log port read.
 * Once the vt registers its console ring, kernel log and console output are
 * published there INSTEAD of being transmitted: the vt owns COM1 from then on
 * and writes the serial-bound slot through serial_owner_write.  Text the ring
 * cannot take, and every unlocked (panic/NMI) write, still goes straight to COM1.
 * The state on the logging path (lock, console ring, klog ring, kmsg ring) is
 * reached through *_slot() accessors that add the kernel higher-half offset once
 * serial_enable_high_alias(1) is set, so logging works under a root with no low
 * identity mapping. The input ring below has no such accessor. */
#include <stdarg.h>
#include "console_ring.h"
#include "ipc.h"
#include "capability.h"
#include "kmsg.h"
#include "memory.h"
#include "physmem.h"
#include "serial.h"
#include "stdio.h"

#include "process.h"
#include "sync/spinlock.h"
#include "paging.h"
#include "xfer_buffer.h"
#include "wasmos/ringbuf.h"
#include "wasmos_driver_abi.h"
#include "wasmos_status.h"
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

/* The kernel log ring (kmsg.h): every kernel-log write since the first line of
 * kmain.  Appended under g_serial_lock, or unlocked on the panic path. */
static kmsg_t g_kmsg;
/* Who is woken on a kernel-log append (kmsg.h).  Marked under the same lock as
 * the append; delivered by kmsg_poll(). */
static kmsg_notify_t g_kmsg_notify;

static ksync_spinlock_t g_serial_lock = {0};
static uint8_t g_serial_high_alias_enabled = 0;
static uint32_t g_console_ring_id = 0;
static console_ring_t* g_console_ring = 0;

/* klog ring: physical base + byte size of the VT-owned SPSC ring.
 * phys == 0 means no VT ring is registered yet, so console_ring_publish takes nothing
 * and early boot keeps flowing only to console_ring + COM1 TX. */
static uint64_t g_klog_ring_phys = 0;
static uint32_t g_klog_ring_bytes = 0;
/* Doorbell for the ring's consumer.  console_ring_publish runs under g_serial_lock,
 * possibly in interrupt context, so it may not send IPC itself; it only raises
 * g_klog_doorbell_pending, and klog_poll() delivers the notify from the scheduler
 * loop, where no serial lock is held.  Endpoint 0 means no consumer registered a
 * doorbell, which keeps the whole path inert. */
static uint32_t g_klog_notify_endpoint = 0;
static volatile uint8_t g_klog_doorbell_pending = 0;
/* Context that registered the ring.  It drains every record and owns COM1 for
 * the text it was handed, so it is the only caller serial_owner_write accepts. */
static uint32_t g_klog_ring_owner = 0;
/* Kernel-log port: the I/O base of a second UART that carries the kernel log and
 * nothing else, written by the kernel directly on every kernel-log write --
 * whatever the vt is doing, whichever slot serial is bound to.  0 means none.
 * Set once from the `log.serial` boot option (serial_log_port_enable). */
static uint16_t g_log_port = 0;

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
    uintptr_t addr = (uintptr_t)&g_console_ring_id;
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

static inline uint32_t* serial_klog_notify_endpoint_slot(void) {
    uintptr_t addr = (uintptr_t)&g_klog_notify_endpoint;
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (uint32_t*)(void*)addr;
}

static inline uint32_t* serial_klog_ring_owner_slot(void) {
    uintptr_t addr = (uintptr_t)&g_klog_ring_owner;
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (uint32_t*)(void*)addr;
}

static inline uint16_t* serial_log_port_slot(void) {
    uintptr_t addr = (uintptr_t)&g_log_port;
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (uint16_t*)(void*)addr;
}

static inline volatile uint8_t* serial_klog_doorbell_slot(void) {
    uintptr_t addr = (uintptr_t)&g_klog_doorbell_pending;
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (volatile uint8_t*)(void*)addr;
}

static inline kmsg_t* serial_kmsg(void) {
    uintptr_t addr = (uintptr_t)&g_kmsg;
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (kmsg_t*)(void*)addr;
}

static inline kmsg_notify_t* serial_kmsg_notify(void) {
    uintptr_t addr = (uintptr_t)&g_kmsg_notify;
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (kmsg_notify_t*)(void*)addr;
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

/* One byte to the kernel-log port.  The wait for THR-empty is bounded: a UART
 * that stops draining drops log bytes rather than wedging every kernel-log
 * writer behind it. */
static void log_port_put_char(uint16_t port, char c) {
    for (uint32_t spin = 0; spin < 100000u && (inb((uint16_t)(port + 5)) & 0x20) == 0; ++spin) {
    }
    outb(port, (uint8_t)c);
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
    /* One plain page. The ring was a shared region while a driver could map it
     * by id; nothing outside this file reads it now, so it needs no id, no
     * grants and no refcount. */
    const uint64_t phys_base = pfa_alloc_pages(1);
    if (phys_base == 0) {
        return;
    }
    *ring_id_slot = 0;
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

/* The console ring's raw backing address: a PHYSICAL
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

/* Publish `s` into the vt's console ring as records for `slot`
 * (VT_CONSOLE_SLOT_KLOG for kernel log), at most VT_CONSOLE_RECORD_TEXT_MAX bytes
 * of text per record.  Returns how many leading bytes of `s` were published:
 * all of them, or fewer when no ring is registered or the ring is full.  The
 * caller transmits the remainder itself, so text is never lost -- only, on a
 * full ring, reordered ahead of what the vt has yet to drain.  Raises the
 * doorbell when anything was published. */
static uint32_t console_ring_publish(uint8_t slot, const char* s) {
    uint64_t phys = *serial_klog_ring_phys_slot();
    if (!phys || !s) {
        return 0;
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
        return 0;
    }
    uint8_t record[1u + VT_CONSOLE_RECORD_TEXT_MAX];
    uint32_t published = 0;
    record[0] = slot;
    while (s[published]) {
        uint32_t n = 0;
        while (n < VT_CONSOLE_RECORD_TEXT_MAX && s[published + n]) {
            record[1u + n] = (uint8_t)s[published + n];
            n++;
        }
        if (wasmos_ringbuf_write_record(&rb, record, 1u + n) < 0) {
            break;
        }
        published += n;
    }
    if (published != 0) {
        /* Raise the doorbell only; klog_poll() sends it.  See the declaration. */
        *serial_klog_doorbell_slot() = 1;
    }
    return published;
}

int console_ring_drained(void) {
    uint64_t phys = *serial_klog_ring_phys_slot();
    if (!phys) {
        return 1;
    }
    if (g_serial_high_alias_enabled && phys < KERNEL_HIGHER_HALF_BASE) {
        phys += KERNEL_HIGHER_HALF_BASE;
    }
    wasmos_ringbuf_t rb;
    if (wasmos_ringbuf_attach(&rb, ptr_cast(void, phys), *serial_klog_ring_bytes_slot()) != 0) {
        return 1;
    }
    return wasmos_ringbuf_is_empty(&rb) ? 1 : 0;
}

/* Deliver a pending klog doorbell to the registered consumer.  Called from the
 * scheduler loop (kernel_boot_run_scheduler_loop), which is the nearest point to
 * the logging path that holds no serial lock and is not interrupt context.
 *
 * The flag is cleared before the send, so a klog write racing the send re-arms
 * the doorbell rather than being swallowed.  A failed send is dropped silently:
 * the consumer's queue being full means it has messages to process and will
 * drain the ring on that wake anyway, and logging the failure here would write
 * klog from inside the klog doorbell path. */
void klog_poll(void) {
    if (!*serial_klog_doorbell_slot()) {
        return;
    }
    uint32_t endpoint = *serial_klog_notify_endpoint_slot();
    if (endpoint == 0) {
        *serial_klog_doorbell_slot() = 0;
        return;
    }
    *serial_klog_doorbell_slot() = 0;
    ipc_message_t msg;
    msg.type = (uint32_t)VT_IPC_KLOG_NOTIFY;
    msg.request_id = 0;
    msg.source = IPC_ENDPOINT_NONE;
    msg.destination = endpoint;
    msg.arg0 = 0;
    msg.arg1 = 0;
    msg.arg2 = 0;
    msg.arg3 = 0;
    (void)ipc_send_from(IPC_CONTEXT_KERNEL, endpoint, &msg);
}

/* Adopts an already-initialised ringbuf, owned by owner_context_id as
 * xfer-buffer `id`, as the console ring: from then on kernel log and console
 * output are published there as records and not transmitted, and
 * owner_context_id owns COM1 for that text (serial_owner_write).  Only one ring
 * is tracked; a second successful call replaces the first, and there is no
 * unregister.
 *
 * Only a holder of the console.owner capability (CAP_CONSOLE_OWNER, granted to
 * the vt by its manifest) may register: the ring receives every tty's output
 * and the kernel log, and its registrant owns COM1.  Owning a buffer and an
 * endpoint is not authority over the console.
 *
 * Returns 0 once the ring is live; WASMOS_ERR_KERNEL_NOT_AUTHORIZED for a
 * caller without console.owner; WASMOS_INVAL for a zero id, a zero base or
 * size, or a region without a valid ringbuf header; xfer_buffer_describe's code
 * for an id that is not a BUFFER_KIND_TRANSFER buffer of that owner;
 * WASMOS_ERR_KERNEL_UNALIGNED for a base that is not page-aligned; and
 * WASMOS_ERR_KERNEL_NOT_AUTHORIZED for a notify endpoint the owner does not own.
 * The rejection matters: an accepted bad region would corrupt memory on every
 * subsequent write. */
int klog_register_ring(uint32_t owner_context_id, uint32_t id, uint32_t notify_endpoint) {
    if (!capability_has(owner_context_id, CAP_CONSOLE_OWNER)) {
        return WASMOS_ERR_KERNEL_NOT_AUTHORIZED;
    }
    if (id == 0) {
        return WASMOS_INVAL;
    }
    /* The doorbell may only name an endpoint the registering context owns, so a
     * caller cannot point kernel notifies at a third party's endpoint. */
    if (notify_endpoint != 0) {
        uint32_t notify_owner = 0;
        if (ipc_endpoint_owner(notify_endpoint, &notify_owner) != IPC_OK ||
            notify_owner != owner_context_id) {
            return WASMOS_ERR_KERNEL_NOT_AUTHORIZED;
        }
    }
    /* The VT owns the ring as a BUFFER_KIND_TRANSFER xfer-buffer (the same
     * zero-copy transport the socket rings use); resolve it by (id, owner) and
     * take its physical base.  No pin/retain: the VT holds the buffer for its
     * whole lifetime, matching the kernel's fixed-region console_ring model. */
    xfer_buffer_t buf;
    int32_t describe_rc = xfer_buffer_describe(id, BUFFER_KIND_TRANSFER, owner_context_id, &buf);
    if (describe_rc != WASMOS_ERR_NONE) {
        return describe_rc;
    }
    uint64_t base = xfer_buffer_object_phys(&buf);
    uint32_t region_bytes = buf.size_bytes;
    if (base == 0 || region_bytes == 0) {
        return WASMOS_INVAL;
    }
    if ((base & 0xFFFull) != 0) {
        return WASMOS_ERR_KERNEL_UNALIGNED;
    }
    uint64_t alias = base;
    if (g_serial_high_alias_enabled && alias < KERNEL_HIGHER_HALF_BASE) {
        alias += KERNEL_HIGHER_HALF_BASE;
    }
    /* Validate the region is an initialized ringbuf before accepting it, so a
     * bad/foreign id cannot wedge klog output. */
    wasmos_ringbuf_t rb;
    if (wasmos_ringbuf_attach(&rb, ptr_cast(void, alias), region_bytes) != 0) {
        return WASMOS_INVAL;
    }
    *serial_klog_ring_bytes_slot() = region_bytes;
    *serial_klog_notify_endpoint_slot() = notify_endpoint;
    *serial_klog_ring_owner_slot() = owner_context_id;
    /* Publish phys last: console_ring_publish reads phys first, so a non-zero phys
     * always implies a valid bytes value and doorbell endpoint are visible. */
    *serial_klog_ring_phys_slot() = base;
    return 0;
}

void klog_release_ring(uint32_t owner_context_id) {
    if (owner_context_id == 0 || *serial_klog_ring_owner_slot() != owner_context_id) {
        return;
    }
    /* Under the serial lock: console_ring_publish runs under it, so no publish
     * is still writing into the ring once this returns. phys goes first, which
     * is what every reader checks. */
    ksync_spinlock_lock(serial_lock_ptr());
    preempt_disable();
    *serial_klog_ring_phys_slot() = 0;
    *serial_klog_notify_endpoint_slot() = 0;
    *serial_klog_ring_owner_slot() = 0;
    preempt_enable();
    ksync_spinlock_unlock(serial_lock_ptr());
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
 * kmsg ring only. */
void serial_init(void) {
    if (g_serial_driver && g_serial_driver->init) {
        g_serial_driver->init();
    }
    serial_ring_init();
}

/* Common body of every console writer: the legacy console ring always sees `s`;
 * kernel log is appended to the kmsg ring and transmitted on the kernel-log port
 * when one is enabled; with `route` set it is then offered to the vt's console
 * ring as records for `slot`, and only what the ring did not take is transmitted
 * on COM1 with LF->CRLF expansion.  Without `route` all of it is transmitted.
 *
 * Takes no lock.  Preemption is disabled across the whole body, so a preemptible
 * caller cannot be descheduled part-way through a line.  A NULL string is
 * ignored, and a string literal in the kernel image is rebased onto the
 * higher-half alias when that alias is armed. */
static void serial_emit(uint8_t slot, const char* s, int route) {
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
    preempt_disable();
    serial_ring_write(s);
    if (slot == VT_CONSOLE_SLOT_KLOG) {
        kmsg_append(serial_kmsg(), s);
        kmsg_notify_mark(serial_kmsg_notify());
    }
    uint16_t log_port = *serial_log_port_slot();
    if (log_port != 0 && slot == VT_CONSOLE_SLOT_KLOG) {
        for (const char* p = s; *p; ++p) {
            if (*p == '\n') {
                log_port_put_char(log_port, '\r');
            }
            log_port_put_char(log_port, *p);
        }
    }
    /* TODO: a slot writer's text that the full ring cannot take goes to COM1 here
     * and never reaches its slot's screen; console_write needs backpressure for a
     * writer with a controlling slot (docs/TASKS.md). */
    const char* rest = route ? s + console_ring_publish(slot, s) : s;
    for (const char* p = rest; *p; ++p) {
        if (*p == '\n') {
            serial_transmit('\r');
        }
        serial_transmit(*p);
    }
    preempt_enable();
}

/* The normal console writer: takes g_serial_lock so concurrent CPUs cannot
 * interleave their output, then emits `s` as kernel log -- through the vt's
 * console ring once one is registered, on COM1 before that.
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
    serial_emit(VT_CONSOLE_SLOT_KLOG, s, 1);
    ksync_spinlock_unlock(serial_lock_ptr());
}

/* Console output of a process whose controlling slot is `tty`: published to the
 * vt's console ring for that slot, so it lands on the slot and reaches serial
 * only when the vt mirrors that slot there.  `tty` 0 -- no controlling slot -- is
 * kernel log, exactly serial_write.  Same lock, context rules and fallback as
 * serial_write: with no ring registered, or one too full to take the text, it
 * is transmitted on COM1. */
void console_write_tty(uint32_t tty, const char* s) {
    if (!s) {
        return;
    }
    uint8_t slot = (tty > 0xFFu) ? (uint8_t)VT_CONSOLE_SLOT_KLOG : (uint8_t)tty;
    ksync_spinlock_lock(serial_lock_ptr());
    serial_emit(slot, s, 1);
    ksync_spinlock_unlock(serial_lock_ptr());
}

/* COM1 output on behalf of the console ring's owner: the vt writing the
 * serial-bound slot.  Nothing here reaches the console rings or the kmsg ring, since the
 * text came out of the console ring in the first place.  LF is expanded to CRLF.
 *
 * Returns 0, or WASMOS_ERR_KERNEL_NOT_AUTHORIZED when no console ring is
 * registered or context_id is not the context that registered it. */
int32_t serial_owner_write(uint32_t context_id, const char* s, uint32_t len) {
    if (*serial_klog_ring_phys_slot() == 0 || context_id == 0 ||
        context_id != *serial_klog_ring_owner_slot()) {
        return WASMOS_ERR_KERNEL_NOT_AUTHORIZED;
    }
    ksync_spinlock_lock(serial_lock_ptr());
    preempt_disable();
    for (uint32_t i = 0; i < len; ++i) {
        if (s[i] == '\n') {
            serial_transmit('\r');
        }
        serial_transmit(s[i]);
    }
    preempt_enable();
    ksync_spinlock_unlock(serial_lock_ptr());
    return 0;
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

/* The write path -- console ring, kmsg ring, COM1 TX with LF->CRLF
 * expansion -- with the lock deliberately NOT taken, and never through the vt's
 * console ring: this always transmits.
 *
 * It exists for contexts that cannot wait on g_serial_lock: a panic or exception
 * handler may have interrupted a CPU that is holding it, and an NMI can arrive
 * while this very CPU holds it, so taking it there deadlocks instead of
 * printing.  Those are also the contexts in which the vt may never drain its
 * ring again, which is why the text goes straight to the wire.  The cost is
 * that output from another CPU can interleave mid-string, and a concurrent
 * writer can corrupt the ring positions; a garbled panic line is the accepted
 * trade for one that appears at all. */
void serial_write_unlocked(const char* s) {
    serial_emit(VT_CONSOLE_SLOT_KLOG, s, 0);
}

/* Programs the UART at I/O base `port` (115200 8N1, FIFO on, interrupts off) as
 * the kernel-log port and replays the kmsg ring into it, CR-expanded, so the port
 * starts with the boot output written before it existed.  From then on every
 * kernel-log write -- locked or unlocked, routed through the vt or not -- is also
 * transmitted there.  Console output of processes with a controlling slot is not.
 *
 * Returns 0, or WASMOS_NOENT when no UART answers at `port` (its scratch register
 * does not hold a written value), in which case nothing changes.  Call it once,
 * early, before other CPUs log. */
int32_t serial_log_port_enable(uint16_t port) {
    outb((uint16_t)(port + 7), 0x5A);
    if (inb((uint16_t)(port + 7)) != 0x5A) {
        return WASMOS_NOENT;
    }
    outb((uint16_t)(port + 1), 0x00); /* interrupts off */
    outb((uint16_t)(port + 3), 0x80); /* DLAB */
    outb((uint16_t)(port + 0), 0x01); /* divisor low: 115200 baud */
    outb((uint16_t)(port + 1), 0x00); /* divisor high */
    outb((uint16_t)(port + 3), 0x03); /* 8N1 */
    outb((uint16_t)(port + 2), 0xC7); /* FIFO on, cleared, 14-byte threshold */
    outb((uint16_t)(port + 4), 0x03); /* DTR + RTS, OUT2 clear: no IRQ */

    ksync_spinlock_lock(serial_lock_ptr());
    kmsg_t* k = serial_kmsg();
    if (kmsg_first(k) != 0) {
        static const char k_lost[] = "[serial] log port: earlier boot output not retained\r\n";
        for (const char* p = k_lost; *p; ++p) {
            log_port_put_char(port, *p);
        }
    }
    uint32_t count = kmsg_size(k);
    uint8_t chunk[64];
    for (uint32_t off = 0; off < count; off += (uint32_t)sizeof(chunk)) {
        uint32_t n = count - off < (uint32_t)sizeof(chunk) ? count - off : (uint32_t)sizeof(chunk);
        kmsg_copy(k, chunk, off, n);
        for (uint32_t i = 0; i < n; ++i) {
            if (chunk[i] == '\n') {
                log_port_put_char(port, '\r');
            }
            log_port_put_char(port, (char)chunk[i]);
        }
    }
    *serial_log_port_slot() = port;
    ksync_spinlock_unlock(serial_lock_ptr());
    return 0;
}

/* Bytes of kernel log currently retrievable through serial_early_log_copy,
 * saturating at KMSG_SIZE once the ring has wrapped.  Read without the lock, so it
 * can grow between this call and the copy. */
uint32_t serial_early_log_size(void) {
    return kmsg_size(serial_kmsg());
}

/* Copies up to len bytes of the kernel log into dst, where offset 0 is the
 * OLDEST retained byte rather than a buffer index (kmsg_copy).  Takes no lock,
 * so a concurrent kernel-log write can tear the result; serial_kmsg_read is the
 * consistent reader. */
void serial_early_log_copy(uint8_t* dst, uint32_t offset, uint32_t len) {
    kmsg_copy(serial_kmsg(), dst, offset, len);
}

/* kmsg_read under g_serial_lock, so the copied bytes are one consistent range of
 * the kernel log.  dst must be kernel memory that cannot fault: the lock holds
 * interrupts off, so a caller copying to a guest bounces through a stack buffer
 * and keeps len small. */
int32_t serial_kmsg_read(uint64_t* seq, uint8_t* dst, uint32_t len) {
    ksync_spinlock_lock(serial_lock_ptr());
    int32_t rc = kmsg_read(serial_kmsg(), seq, dst, len);
    ksync_spinlock_unlock(serial_lock_ptr());
    return rc;
}

static void serial_kmsg_env_lock(void* user) {
    (void)user;
    ksync_spinlock_lock(serial_lock_ptr());
}

static void serial_kmsg_env_unlock(void* user) {
    (void)user;
    ksync_spinlock_unlock(serial_lock_ptr());
}

/* A subscription is alive while its context still owns its endpoint: endpoints
 * are released when their process exits. */
static int serial_kmsg_env_alive(void* user, uint32_t context_id, uint32_t endpoint) {
    uint32_t owner = 0;
    (void)user;
    return ipc_endpoint_owner(endpoint, &owner) == IPC_OK && owner == context_id;
}

static int serial_kmsg_env_send(void* user, uint32_t endpoint) {
    (void)user;
    ipc_message_t msg;
    msg.type = (uint32_t)WASMOS_IPC_KMSG_NOTIFY;
    msg.request_id = 0;
    msg.source = IPC_ENDPOINT_NONE;
    msg.destination = endpoint;
    msg.arg0 = 0;
    msg.arg1 = 0;
    msg.arg2 = 0;
    msg.arg3 = 0;
    return ipc_send_from(IPC_CONTEXT_KERNEL, endpoint, &msg) == IPC_OK ? 0 : 1;
}

/* The kernel side of the kmsg subscriber protocol (kmsg.h): the serial lock
 * guards the table, the IPC layer answers liveness and carries the notify. */
static const kmsg_notify_env_t g_kmsg_notify_env = {
    serial_kmsg_env_lock,
    serial_kmsg_env_unlock,
    serial_kmsg_env_alive,
    serial_kmsg_env_send,
    0,
};

/* Subscribes `endpoint`, which context_id must own, to kernel-log appends
 * (kmsg_notify_subscribe).  Returns 0, WASMOS_ERR_KERNEL_NOT_AUTHORIZED for an
 * endpoint context_id does not own, WASMOS_INVAL, or
 * WASMOS_ERR_KERNEL_NO_SUBSCRIBER_SLOT. */
int32_t serial_kmsg_subscribe(uint32_t context_id, uint32_t endpoint) {
    if (endpoint == 0 || !serial_kmsg_env_alive(0, context_id, endpoint)) {
        return WASMOS_ERR_KERNEL_NOT_AUTHORIZED;
    }
    return kmsg_notify_subscribe(serial_kmsg_notify(), &g_kmsg_notify_env, context_id, endpoint);
}

/* Delivers owed kmsg notifies (kmsg_notify_deliver).  Called from the scheduler
 * loop next to klog_poll(), for the same reasons: it is not interrupt context,
 * and the notify is sent with no serial lock held.  A notify that could not be
 * queued stays owed and is retried here on a later pass. */
void kmsg_poll(void) {
    /* Unlocked first look: this runs on every scheduler pass.  A flag raised just
     * after it is seen on the next pass. */
    if (!__atomic_load_n(&serial_kmsg_notify()->pending, __ATOMIC_RELAXED)) {
        return;
    }
    kmsg_notify_deliver(serial_kmsg_notify(), &g_kmsg_notify_env);
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
