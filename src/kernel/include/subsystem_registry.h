#ifndef WASMOS_SUBSYSTEM_REGISTRY_H
#define WASMOS_SUBSYSTEM_REGISTRY_H

#include <stddef.h>
#include <stdint.h>
#include "../../drivers/include/wasmos_driver_abi.h"

/* Registry of the subsystems that can run an executable, keyed by request tag,
 * plus the exec-format handlers that decide which tag a given file belongs to.
 * Entries come either from the kernel itself (built-ins, registered at startup)
 * or from user-space broker services over IPC.
 *
 * One global spinlock covers both tables; every function below takes it
 * internally and none of them block. Both lookups return a pointer AFTER
 * dropping that lock -- see the lifetime note on
 * wasmos_subsystem_registry_find. */

struct wasmos_subsystem_ops;
typedef struct wasmos_subsystem_ops wasmos_subsystem_ops_t;

/* Who executes a module for this tag: kernel code reached through `ops`, or a
 * user-space broker service reached over `broker_endpoint`. */
typedef enum {
    WASMOS_SUBSYSTEM_HANDLER_BUILTIN = 0,
    WASMOS_SUBSYSTEM_HANDLER_BROKER = 1,
} wasmos_subsystem_handler_kind_t;

/* Registration caps.  Broker subsystems and exec-format handlers are registered
 * by user-space services over IPC, so the registry bounds both the global count
 * and the per-owner count to keep one process from monopolizing the tables. */
#define WASMOS_SUBSYSTEM_MAX_BROKERS 8u
#define WASMOS_SUBSYSTEM_MAX_BROKERS_PER_OWNER 4u
#define WASMOS_EXEC_HANDLER_MAX 16u
#define WASMOS_EXEC_HANDLER_MAX_PER_OWNER 8u

/* What an exec handler's match tree is evaluated against. All fields are
 * borrowed for the duration of the lookup; initial_bytes holds the first
 * initial_size bytes of the file, which is bounded by
 * wasmos_subsystem_registry_exec_max_probe_bytes. */
typedef struct {
    const char* path;
    const char* filename;
    const uint8_t* initial_bytes;
    uint32_t initial_size;
} wasmos_exec_probe_t;

/* One registered subsystem. Tags are NUL-terminated and validated at
 * registration; request_tag is the key and is unique across the table. */
typedef struct wasmos_subsystem_registry_entry {
    char request_tag[WASMOS_SUBSYSTEM_TAG_LEN + 1]; /* what a spawn asks for */
    char runtime_tag[WASMOS_SUBSYSTEM_TAG_LEN + 1]; /* what `ps` reports for the child */
    char broker_name[WASMOS_SUBSYSTEM_TAG_LEN + 1]; /* empty for a built-in */
    wasmos_subsystem_handler_kind_t kind;
    uint8_t uses_wasm_payload;        /* the image is a wasm module */
    uint8_t needs_runtime_lock;       /* serialise the child's entry calls */
    uint8_t gates_ready_for_services; /* the child must notify_ready explicitly */
    uint32_t broker_endpoint;         /* meaningful for BROKER entries only */
    uint32_t owner_context_id; /* 0 = kernel built-in; nonzero = registering broker context */
    const wasmos_subsystem_ops_t* ops;            /* BUILTIN only; NULL for a broker */
    struct wasmos_subsystem_registry_entry* next; /* hash-bucket chain */
} wasmos_subsystem_registry_entry_t;

/* One registered executable-format handler: a match tree over a file's name and
 * leading bytes, plus the subsystem to route a match to. */
typedef struct wasmos_exec_handler_registry_entry {
    char handler_name[WASMOS_EXEC_HANDLER_NAME_LEN + 1];
    /* Copied from the owning subsystem entry at registration; a later change
     * there is not reflected here. */
    char request_tag[WASMOS_SUBSYSTEM_TAG_LEN + 1];
    char runtime_tag[WASMOS_SUBSYSTEM_TAG_LEN + 1];
    char broker_name[WASMOS_SUBSYSTEM_TAG_LEN + 1];
    uint32_t broker_endpoint;
    uint32_t priority;        /* higher wins; ties broken by name, so order is stable */
    uint32_t max_probe_bytes; /* leading bytes this tree may inspect */
    uint32_t node_count;
    uint32_t root_index;             /* index into nodes[] where evaluation starts */
    uint32_t owner_context_id;       /* registering broker context; dropped when it exits */
    wasmos_exec_match_node_t* nodes; /* owned by the registry; freed with the entry */
    struct wasmos_exec_handler_registry_entry* next;
} wasmos_exec_handler_registry_entry_t;

/* Register a kernel built-in for `request_tag`. Returns 0, or -1 for NULL
 * arguments, a tag that fails validation, a request_tag already registered
 * (duplicates are refused, not replaced), or an allocation failure. Built-in
 * entries are owned by context 0 and are never dropped. */
int wasmos_subsystem_registry_register_builtin(const char* request_tag, const char* runtime_tag,
                                               uint8_t uses_wasm_payload,
                                               uint8_t needs_runtime_lock,
                                               uint8_t gates_ready_for_services,
                                               const wasmos_subsystem_ops_t* ops);
/* Register a user-space broker for `request_tag`, reachable at
 * `broker_endpoint`. Same duplicate and validation rules as the built-in form,
 * plus the WASMOS_SUBSYSTEM_MAX_BROKERS / _PER_OWNER caps -- exceeding either
 * gives -1. The caller (PM) is what verifies that owner_context_id actually
 * owns broker_endpoint; the registry does not re-check it. Dropped
 * automatically when that context exits. */
int wasmos_subsystem_registry_register_broker(const char* request_tag, const char* runtime_tag,
                                              const char* broker_name, uint32_t broker_endpoint,
                                              uint32_t owner_context_id, uint8_t uses_wasm_payload,
                                              uint8_t needs_runtime_lock,
                                              uint8_t gates_ready_for_services);
/* Register an exec-format handler routing to `request_tag`, which must already
 * name a BROKER subsystem. `nodes[0..node_count)` is COPIED into registry-owned
 * storage, so the caller keeps its own. Returns 0, or -1 for NULL arguments, a
 * match tree that fails validation against max_probe_bytes, a missing or
 * non-broker owner, a duplicate (handler_name, request_tag), the
 * WASMOS_EXEC_HANDLER_MAX / _PER_OWNER caps, or an allocation failure. */
int wasmos_subsystem_registry_register_exec_handler(const char* handler_name,
                                                    const char* request_tag,
                                                    uint32_t owner_context_id, uint32_t priority,
                                                    uint32_t max_probe_bytes,
                                                    const wasmos_exec_match_node_t* nodes,
                                                    uint32_t node_count, uint32_t root_index);
/* Remove every broker subsystem and exec handler owned by owner_context_id.
 * Called from process teardown so a dead broker leaves no stale endpoint.
 * Context 0 (the kernel built-ins) is ignored, so built-ins survive. */
void wasmos_subsystem_registry_drop_owner(uint32_t owner_context_id);
/* Copies the entry for `request_tag` into *out and returns 0, or returns -1 when
 * no entry matches (leaving *out untouched) or either argument is NULL.
 *
 * The copy is the contract, not a convenience: a BROKER entry is freed by
 * wasmos_subsystem_registry_drop_owner when its registering context exits, so a
 * pointer handed out from under the registry lock can be dangling by the time the
 * caller reads it. out->next is cleared because the bucket chain belongs to the
 * registry; out->ops is copied as-is and remains valid, being NULL for a broker
 * and a static table for a built-in. */
int wasmos_subsystem_registry_find(const char* request_tag, wasmos_subsystem_registry_entry_t* out);
/* Copies the highest-priority exec handler whose match tree accepts `probe` into
 * *out and returns 0, or returns -1 when nothing matches or either argument is
 * NULL.  Ties break on handler_name then request_tag, so the choice is stable.
 *
 * As with wasmos_subsystem_registry_find, the copy is what makes the result safe
 * to hold: an exec handler is owned by the context that registered it and is
 * freed with that context.  out->nodes and out->next are cleared -- the match
 * tree is registry-owned storage and has already served its purpose by the time
 * a caller sees the result. */
int wasmos_subsystem_registry_find_exec_handler(const wasmos_exec_probe_t* probe,
                                                wasmos_exec_handler_registry_entry_t* out);
/* Largest max_probe_bytes any registered handler declared -- how many leading
 * bytes a caller needs to read before probing. 0 when no handler is registered,
 * and it does NOT fall when handlers are dropped, so it is an upper bound
 * rather than an exact figure. */
uint32_t wasmos_subsystem_registry_exec_max_probe_bytes(void);
/* Drop every entry, built-ins included, and free both tables. Intended for
 * tests: it invalidates pointers returned by either lookup. */
void wasmos_subsystem_registry_reset(void);

#endif
