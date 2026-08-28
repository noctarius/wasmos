/* process_manager_internal.h - PM internal state: g_pm struct, service table, and
 * buffer descriptors shared across process_manager.cpp,
 * process_manager_spawn.c and process_manager_services.c.
 *
 * CONCURRENCY. There is no lock anywhere in this header, and that is the whole
 * design: g_pm and everything reachable from it are touched only from the PM
 * process's own entry point, which the scheduler runs on one CPU at a time.
 * The two exceptions are the endpoint fields, which other CPUs read through
 * process_manager_*_endpoint() and which therefore go through
 * pm_atomic_load_u32 / pm_atomic_store_u32; and process_manager_on_child_ready,
 * which runs on a native driver's CPU and for that reason is forbidden to touch
 * g_pm.spawn at all. Any new cross-CPU reader has the same obligation.
 *
 * RETURN CONVENTION. Every pm_handle_* below is an IPC request handler with the
 * same contract: 0 means handled (the handler has already sent its own reply),
 * non-zero means it did not, and the PM run loop turns that value into a
 * PROC_IPC_ERROR / SVC_IPC_ERROR carrying it in arg1. The values are negative
 * packed abi/errors.yaml codes (WASMOS_ERR_PROC_PM_*) except in
 * process_manager_services.c's SVC_* handlers, which still return a bare -1.
 * None of them block: a request that cannot be answered immediately parks its
 * state in g_pm.spawn or g_pm.waits and is completed by a later poll. */
#ifndef WASMOS_PROCESS_MANAGER_INTERNAL_H
#define WASMOS_PROCESS_MANAGER_INTERNAL_H

#include <stdint.h>
#include "boot.h"
#include "ipc.h"
#include "list.h"
#include "process.h"
#include "wasmos_app.h"
#include "wasmos_driver_abi.h"
#include "xfer_buffer.h"

/* TODO: dead constant. xfer_buffer.c documents its own TRANSFER capacity as
 * mirroring this, but nothing in the tree reads it, so the two can drift
 * silently. Either make xfer_buffer.c derive its capacity from here or delete
 * this. */
#define PM_XFER_BUFFER_SIZE (2u * 1024u * 1024u)
/* Max DMA windows in one spawn profile. Must not exceed
 * CAPABILITY_DMA_WINDOW_LIMIT, which sizes the capability-side array the
 * profile is copied into. */
#define PM_DMA_WINDOW_LIMIT 16u

/* One WASMOS-APP the PM has launched and still tracks. Slots live in the
 * g_pm.apps list and are recycled: in_use == 0 marks a free one, and
 * pm_slot_reset both frees any owned blob and clears the slot. The state is
 * `arg` for the child's pm_app_entry, so it must outlive the child. */
typedef struct {
    uint8_t in_use;
    uint32_t pid;
    uint32_t flags; /* wasmos_app_desc_t::flags, filled on first entry */
    /* The app image the child parses. Either borrowed (a boot module, valid for
     * the life of the kernel) or owned -- see owned_blob_storage below. */
    const uint8_t* blob;
    uint32_t blob_size;
    /* Non-NULL when the PM copied the image into pages it allocated itself,
     * because the source (an FS read into a shared transfer buffer) is reused
     * before the child runs. `blob` then aliases this. Released by
     * pm_slot_release_owned_blob via pfa_free_pages. */
    uint8_t* owned_blob_storage;
    uint64_t owned_blob_storage_phys;
    uint32_t owned_blob_storage_pages;
    uint8_t started; /* the child's first entry call has run */
    /* WASM entry signature, part of the module ABI: argc is always 4 and every
     * arg is 0. Real startup values reach the child through its spawn-info
     * buffer and svc_lookup, not through these. */
    uint32_t entry_argc;
    uint32_t entry_arg0;
    uint32_t entry_arg1;
    uint32_t entry_arg2;
    uint32_t entry_arg3;
    /* argv blob copied verbatim into the child's spawn-info buffer. Truncation
     * is the caller's problem: the request path bounds the length to this
     * array before it is stored. */
    uint32_t spawn_cli_args_len;
    /* PROC_SPAWN_PATH_TTY encoding: 0 = let pm_alloc_cli_tty pick, otherwise the
     * controlling tty is (requested_tty - 1).  Only meaningful for a child whose
     * manifest sets wants_tty. */
    uint32_t requested_tty;
    char spawn_cli_args[256];
    wasmos_app_instance_t app; /* runtime instance; torn down by wasmos_app_stop */
    char name[64];
} pm_app_state_t;

/* A PROC_IPC_WAIT that could not be answered at once because the child was
 * still alive. Held in the g_pm.waits list until pm_check_waits sees the child
 * exit, replies, and reaps it. owner_context_id is re-verified against the
 * reply endpoint's owner at that point, so a caller that died (or whose
 * endpoint was reused) is dropped rather than answered. */
typedef struct {
    uint8_t in_use;
    uint32_t pid; /* child being waited on */
    uint32_t reply_endpoint;
    uint32_t request_id;
    uint32_t owner_context_id;
} pm_wait_state_t;

/* The single in-flight spawn slot. There is exactly one, so a second
 * synchronous spawn request while in_use is set is refused with
 * WASMOS_ERR_PROC_PM_BUSY rather than queued. Every in-flight spawn is a
 * synchronous one waiting for the child's readiness; the blob read itself is
 * performed inline before the child exists and is never pending here. */
typedef struct {
    uint8_t in_use;
    uint8_t is_sync; /* 1 = SPAWN_SYNC waiting for child readiness */
    uint32_t reply_endpoint;
    uint32_t request_id;
    uint32_t parent_pid;
    uint32_t parent_context_id;
    uint32_t fs_request_id;
    char name[32];
    /* SPAWN_SYNC-only fields (valid when is_sync == 1) */
    uint32_t sync_child_pid;
    uint64_t sync_timeout_ticks; /* deadline tick; 0 = no timeout */
    uint32_t app_flags;          /* desc.flags of spawned app, returned in resp.arg1 */
} pm_spawn_state_t;

/* Hardware access a spawn request asks for, assembled from the request and
 * handed to capability_set_spawn_profile once the child exists. Nothing here is
 * granted until that call succeeds; a rejected profile kills the child. */
typedef struct {
    uint8_t valid;      /* 0 = no caps requested; the profile is not applied */
    uint32_t cap_flags; /* DEVMGR_CAP_* bitmask from the request */
    /* Disjoint I/O windows; the packed-arg spawn opcodes fill exactly one, the
     * descriptor-based path may carry several (see wasmos_io_range_t). */
    uint32_t io_range_count;
    wasmos_io_range_t io_ranges[WASMOS_IO_RANGE_LIMIT];
    uint16_t irq_mask;            /* bit i = IRQ line i; only lines 0..15 exist */
    uint32_t dma_direction_flags; /* WASMOS_DMA_DIR_* */
    uint32_t dma_max_bytes;       /* per-context pinning budget */
    uint32_t dma_window_count;
    wasmos_dma_window_t dma_windows[PM_DMA_WINDOW_LIMIT];
} pm_spawn_caps_t;

/* One entry of the flat name -> endpoint service table.
 *
 * `name` is sized for the LONGEST name any registration path accepts, which is
 * the descriptor path's WASMOS_SVC_NAME_MAX-1 characters plus NUL -- not the 16
 * characters the four packed IPC args of a lookup can carry. Sizing it for the
 * packed path instead truncates a descriptor-registered name into this field,
 * which both loses the NUL (leaving strcmp to read past the entry) and makes two
 * names sharing their first 16 characters collide onto one entry.
 *
 * Re-registration is allowed only by the owning context; another context asking
 * for a name already taken is refused. */
typedef struct {
    uint8_t in_use;
    uint32_t endpoint;
    uint32_t owner_context_id;
    uint32_t flags; /* WASMOS_SVC_FLAG_*, masked to WASMOS_SVC_FLAG_MASK */
    char name[WASMOS_SVC_NAME_MAX];
} pm_service_entry_t;

/* Most participants a shutdown notifies. One slot per CONTEXT that owns a
 * registered service endpoint, not per registered name. */
#define WASMOS_PM_SHUTDOWN_MAX 32u

/* The orderly shutdown sequence's state (process_manager_shutdown.c).
 *
 * `requested` is the only field WRITTEN from another CPU -- the halting
 * process's, through kernel_system_shutdown_arm -- and is accessed with the
 * pm_atomic_* helpers for that reason. Everything else is written by
 * pm_shutdown_step on the PM's own CPU, after it has observed that store.
 *
 * `active` and `index` are READ from the halting process's CPU as well, by the
 * wait loop in kernel_system_shutdown, so they are accessed with the helpers
 * too. The pair is read as one forward-only quantity and never written there. */
typedef struct {
    uint32_t requested; /* armed by the halt/reboot host call */
    uint32_t reason;    /* WASMOS_SHUTDOWN_REASON_* */
    uint32_t active;    /* participants have been collected; read cross-CPU */
    uint8_t pending;    /* the participant at `index` has been notified */
    uint32_t count;
    uint32_t index; /* the participant being waited on */
    uint64_t deadline_ticks;
    uint32_t pids[WASMOS_PM_SHUTDOWN_MAX]; /* descending: reverse spawn order */
    uint32_t endpoints[WASMOS_PM_SHUTDOWN_MAX];
    uint32_t context_ids[WASMOS_PM_SHUTDOWN_MAX];
} pm_shutdown_state_t;

/* The whole of the process manager's state. Single instance (g_pm), zeroed
 * before process_manager_init runs. */
typedef struct {
    const boot_info_t* boot_info; /* borrowed from the bootloader; never freed */
    /* Endpoints the PM owns. All are IPC_ENDPOINT_NONE until the PM's first
     * entry call creates them, and the first five are read from other CPUs
     * through process_manager_*_endpoint(), so they must be accessed with the
     * pm_atomic_* helpers below. */
    uint32_t proc_endpoint; /* well-known request endpoint */
    uint32_t fs_endpoint;   /* the fs.vfs service, once registered */
    uint32_t block_endpoint;
    uint32_t fb_endpoint;
    uint32_t fs_reply_endpoint;     /* replies to the PM's own FS requests */
    uint32_t fs_ctrl_endpoint;      /* drained and discarded each iteration */
    uint32_t broker_reply_endpoint; /* drained synchronously inside the broker
                                     * spawn plan, deliberately NOT in select_id */
    uint32_t select_id;             /* select set over the endpoints above */
    uint32_t fs_request_id;
    uint32_t next_cli_tty; /* round-robin cursor over ttys 1..3 */
    uint8_t started;       /* endpoints and select set exist */
    /* Index of the "sysinit" boot module, or 0xFFFFFFFF when there is none. */
    uint32_t init_module_index;
    uint32_t module_count;
    list_t apps;  /* pm_app_state_t */
    list_t waits; /* pm_wait_state_t */
    pm_spawn_state_t spawn;
    list_t services; /* pm_service_entry_t */
    pm_shutdown_state_t shutdown;
} pm_state_t;

extern pm_state_t g_pm;
/* One-shot latches so each "owner check refused a request" path prints its
 * test marker exactly once instead of on every occurrence. */
extern uint8_t g_pm_wait_owner_deny_logged;
extern uint8_t g_pm_kill_owner_deny_logged;
extern uint8_t g_pm_status_owner_deny_logged;
extern uint8_t g_pm_spawn_owner_deny_logged;

/* Acquire/release access to the endpoint fields of g_pm, which the PM writes on
 * its own CPU and other CPUs read through the process_manager_*_endpoint()
 * accessors. Use these for those fields and nothing else -- the rest of g_pm is
 * PM-private and needs no synchronisation. */
static inline uint32_t pm_atomic_load_u32(const uint32_t* ptr) {
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

static inline void pm_atomic_store_u32(uint32_t* ptr, uint32_t value) {
    __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

/* Service names travel in the four 32-bit IPC args as up to 16 little-endian
 * bytes, NUL-terminated only if shorter. pm_unpack_name_args writes at most
 * out_len-1 characters plus a NUL (a 16-character name is thus truncated in a
 * 16-byte buffer); pm_pack_name_args zero-fills out[4] first and drops anything
 * past 16 characters. Neither reports truncation. */
void pm_unpack_name_args(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, char* out,
                         uint32_t out_len);
void pm_pack_name_args(const char* name, uint32_t out[4]);
/* Next controlling tty for a child that declared WANTS_TTY, cycling 1,2,3,1,...
 * tty 0 is never handed out, and an out-of-range cursor folds back to 1. */
uint32_t pm_alloc_cli_tty(void);

/* Kernel VA of a transfer buffer owned by `owner_context` and named by
 * `buffer_id` (as carried over IPC), or 0 if no such object is owned by that
 * context. Fills *out_size with that object's own size when non-NULL. Used to
 * read a caller-owned buffer that PM does not own. The pointer is a kernel
 * higher-half alias of the object's physical pages: it stays valid while that
 * context owns the buffer, and the PM may write through it (dropping const)
 * only for the request kinds whose contract says the caller supplied the buffer
 * as an output area. */
const uint8_t* pm_foreign_xfer_ptr(uint32_t buffer_id, uint32_t owner_context, uint32_t* out_size);

/* Bind `name` to `endpoint`. Re-binding an existing name is allowed only for
 * its current owner; a different owner is refused. Returns 0, or -1 on an
 * ownership clash or when no slot could be allocated. Names longer than 16
 * characters are truncated into pm_service_entry_t::name. */
/* Advance the shutdown sequence by one step, from the PM's dispatch loop. A
 * no-op until kernel_system_shutdown_arm has been called; does not return once
 * the last participant has answered or been passed over. */
void pm_shutdown_step(uint32_t pm_context_id);
/* Retire the outstanding participant on a WASMOS_IPC_SHUTDOWN_DONE. Ignores a
 * reply from any endpoint other than the one currently waited on, so a late
 * answer from a participant already passed over cannot retire its successor. */
void pm_shutdown_note_done(const ipc_message_t* msg);

int pm_service_set(const char* name, uint32_t endpoint, uint32_t owner_context_id, uint32_t flags);
/* Endpoint bound to `name`, or IPC_ENDPOINT_NONE if the name is unknown. Note
 * the sentinel: 0 is a possible endpoint id, so a zero return is not "absent". */
uint32_t pm_service_lookup(const char* name);
/* The registered service name an endpoint serves, or NULL when it is private.
 * Lock-free for the NMI diagnostic path; see the definition. */
const char* pm_service_name_for_endpoint(uint32_t endpoint);
/* Mirror a registration into the g_pm fast-path endpoint fields for the handful
 * of names the PM itself needs ("fs.vfs", "fs", "block", "vt", "fb"). A plain
 * "fs" registration does NOT displace an already-known fs.vfs endpoint, so
 * path-based spawns stay on the VFS once it exists. Unknown names are ignored. */
void pm_update_well_known_service_endpoint(const char* name, uint32_t endpoint);
/* SVC_IPC_REGISTER_REQ: register msg->source itself under the packed name in
 * arg0..arg3. Returns 0 / -1 per the file-level convention. */
int pm_handle_service_register(uint32_t pm_context_id, const ipc_message_t* msg);
/* SVC_IPC_REGISTER_DESC_REQ: register the endpoint named by a
 * svc_register_desc_t in the caller's transfer buffer (arg1 = byte length,
 * arg2 = buffer_id), replying on msg->source, which must be a DEDICATED reply
 * endpoint distinct from the service endpoint being registered. Refuses a
 * service endpoint owned by anyone other than the reply endpoint's owner, and a
 * v2+ class claim from a context without CAP_SVC_CLASS_REGISTER. */
int pm_handle_service_register_desc(uint32_t pm_context_id, const ipc_message_t* msg);
/* SVC_IPC_LOOKUP_REQ: resolve the packed name in arg0..arg3. An unknown name is
 * still a successful reply -- resp.arg0 carries (uint32_t)-1 rather than an
 * error. */
int pm_handle_service_lookup(uint32_t pm_context_id, const ipc_message_t* msg);
int pm_handle_service_lookup_class(uint32_t pm_context_id, const ipc_message_t* msg);
int pm_handle_class_subscribe(uint32_t pm_context_id, const ipc_message_t* msg);
void pm_services_class_reap(uint32_t pm_context_id);
/* PROC_IPC_SUBSYSTEM_REGISTER_BROKER / PROC_IPC_EXEC_HANDLER_REGISTER: both
 * require CAP_SUBSYSTEM_REGISTER on the caller and a descriptor in the caller's
 * transfer buffer (arg1 = length, arg2 = buffer_id) whose declared endpoint the
 * caller must own. Registrations are dropped automatically when the owning
 * context exits (wasmos_subsystem_registry_drop_owner). */
int pm_handle_subsystem_register_broker(uint32_t pm_context_id, const ipc_message_t* msg);
int pm_handle_exec_handler_register(uint32_t pm_context_id, const ipc_message_t* msg);

/*
 * Spawn handlers. Three axes: how the image is named (boot-module index, or a
 * path read through the FS service), whether hardware capabilities come with
 * it, and whether the reply is immediate or deferred.
 *
 * The plain and _caps forms spawn the child, unpark it, and reply with its pid
 * straight away. The _sync forms reply only once the child announces readiness:
 * they take the single g_pm.spawn slot (returning WASMOS_ERR_PROC_PM_BUSY if it
 * is already taken), return 0 WITHOUT having replied, and are completed later
 * by pm_poll_spawn or by the child's own PROC_IPC_NOTIFY_READY. A caller that
 * asks for a timeout gets a PROC_IPC_ERROR when the deadline passes.
 *
 * Every child is spawned parked, so capabilities, cwd inheritance and auto-reap
 * are applied before its first instruction. A child whose capability profile is
 * rejected is killed rather than started unprivileged.
 */
int pm_handle_spawn(uint32_t pm_context_id, const ipc_message_t* msg);
int pm_handle_spawn_caps(uint32_t pm_context_id, const ipc_message_t* msg);
int pm_handle_spawn_caps_v2(uint32_t pm_context_id, const ipc_message_t* msg);
int pm_handle_spawn_path(uint32_t pm_context_id, const ipc_message_t* msg);
int pm_handle_spawn_path_caps(uint32_t pm_context_id, const ipc_message_t* msg);
int pm_handle_spawn_sync(uint32_t pm_context_id, const ipc_message_t* msg);
int pm_handle_spawn_caps_sync(uint32_t pm_context_id, const ipc_message_t* msg);
int pm_handle_spawn_path_sync(uint32_t pm_context_id, const ipc_message_t* msg);
int pm_handle_spawn_path_caps_sync(uint32_t pm_context_id, const ipc_message_t* msg);
/* PROC_IPC_NOTIFY_READY: latch the sender's ready flag, complete a pending
 * synchronous spawn if this is the child it was waiting for, and then ack the
 * sender. The ack is sent last on purpose -- acking first lets the child resume
 * and destroy its endpoint before the parent has been unblocked. */
int pm_handle_notify_ready(uint32_t pm_context_id, const ipc_message_t* msg);
/* Index of the boot module whose WASMOS-APP name matches, or 0xFFFFFFFF when
 * there is no match, no module list, or the module fails to parse. */
uint32_t pm_find_module_index_by_name(const char* name);
/* Advance the single in-flight spawn: reply when the child reports ready, or
 * fail it if its deadline passed or it died first. No-op when nothing is in
 * flight. Called once per PM dispatch. */
void pm_poll_spawn(uint32_t pm_context_id);
/* Answer every parked PROC_IPC_WAIT whose child has now exited, then reap that
 * child. A waiter whose reply endpoint no longer belongs to the context that
 * registered it is dropped without a reply. Called once per PM dispatch. */
void pm_check_waits(uint32_t pm_context_id);
/* Release the app slots of children that have exited: waits on each (as
 * `owner`, which must be their parent), stops the runtime instance and frees
 * any PM-owned image copy. Children of another parent are left alone. */
void pm_reap_apps(process_t* owner);
/* First free pm_wait_state_t in g_pm.waits, extending the list if none is free.
 * Returns NULL only when the allocation fails. The slot is returned with
 * in_use still 0; the caller sets it. */
pm_wait_state_t* pm_wait_slot_acquire(void);

/* Driver metadata for one boot module. _meta packs the answer into the four
 * reply args; _meta_desc writes a wasmos_module_meta_desc_t into the caller's
 * transfer buffer (arg2 = buffer_id, arg3 = offset) because the region list is
 * variable-length; _meta_path resolves an initfs path to a module index first.
 * All three refuse a module that is not a driver, and an out-of-range match
 * index, with the corresponding WASMOS_ERR_PROC_PM_META_* code. */
int pm_handle_module_meta_desc(uint32_t pm_context_id, const ipc_message_t* msg);
int pm_handle_module_meta_path(uint32_t pm_context_id, const ipc_message_t* msg);

#endif
