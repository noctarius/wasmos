/* wasmos_app.h - WASMOS-APP (.wap) package format and runtime instance management.
 *
 * A .wap package is a binary blob with an 8-byte magic "WASMOSAP", a version field,
 * and a section table that carries the WASM or native ELF payload, linker.metadata
 * (TOML driver match data), and optional resource hints.
 *
 * wasmos_app_parse() extracts the sections into a wasmos_app_desc_t without copying;
 * all name/entry/wasm_bytes pointers alias back into the original blob and remain valid
 * only as long as the blob is live.
 *
 * wasmos_app_start() creates a wasm_driver_t (or native_driver) from the descriptor
 * and handles endpoint resolution and capability granting via injected callbacks. */
#ifndef WASMOS_APP_H
#define WASMOS_APP_H

#include <stdint.h>
#include "ipc.h"
#include "subsystem_registry.h"
#include "wasm_driver.h"

/* First 8 bytes of every .wap blob, compared byte for byte (not NUL-terminated on the
 * wire). */
#define WASMOS_APP_MAGIC "WASMOSAP"
/* Container version the packer (scripts/make_wasmos_app.c) emits, and the only one
 * the parser accepts.  The version is bound to an exact header_size, so a blob whose
 * header_size disagrees is rejected rather than reinterpreted. */
#define WASMOS_APP_VERSION 8u
/* On-wire width of the subsystem tag field (v5+).  Struct fields that hold a parsed tag
 * are this + 1 so the tag is always NUL-terminated in memory. */
#define WASMOS_APP_SUBSYSTEM_TAG_LEN 8u

/* Subsystem tags.  A package requests a tag; the registry maps that request to the
 * runtime tag this build actually provides — "WASM" resolves to "WARP" in a WARP build
 * and to "WASM3" otherwise, so a portable package asks for "WASM".  Tag bytes are
 * restricted to A-Z, 0-9, '+', '_' and '-'. */
#define WASMOS_SUBSYSTEM_TAG_WASM "WASM"
#define WASMOS_SUBSYSTEM_TAG_WASM3 "WASM3"
#define WASMOS_SUBSYSTEM_TAG_WARP "WARP"
#define WASMOS_SUBSYSTEM_TAG_NATIVE "NATIVE"

/* Package type flags stored in the .wap header. */
#define WASMOS_APP_FLAG_DRIVER (1u << 0)
#define WASMOS_APP_FLAG_SERVICE (1u << 1)
#define WASMOS_APP_FLAG_APP (1u << 2)
#define WASMOS_APP_FLAG_NEEDS_PRIV (1u << 3)
/* Native ELF payload; valid for privileged service/driver payloads. */
#define WASMOS_APP_FLAG_NATIVE (1u << 4)
#define WASMOS_APP_FLAG_STORAGE_BOOTSTRAP (1u << 5)
/* Process wants a controlling TTY allocated at spawn; PM fills spawn_info.tty. */
#define WASMOS_APP_FLAG_WANTS_TTY (1u << 6)

/* Wildcard sentinels in a driver-match record: a field set to these matches any device.
 * A v2 package that leaves all of class/subclass/prog_if/vendor/device at the wildcard is
 * read as having no match record at all. */
#define WASMOS_DRIVER_MATCH_ANY_U8 0xFFu
#define WASMOS_DRIVER_MATCH_ANY_U16 0xFFFFu

/* `kind` values of a memory-hint record in the blob.  Only STACK and HEAP are consumed
 * by the parser (as min_pages); the others are parsed for section walking and ignored. */
#define WASMOS_APP_MEM_HINT_LINEAR 0u
#define WASMOS_APP_MEM_HINT_STACK 1u
#define WASMOS_APP_MEM_HINT_HEAP 2u
#define WASMOS_APP_MEM_HINT_IPC 3u
#define WASMOS_APP_MEM_HINT_DEVICE 4u

/* Capacities of the fixed-size tables in wasmos_app_desc_t.  A header count above any of
 * these makes wasmos_app_parse reject the whole package rather than truncate — a
 * truncated table would desynchronise the section walk. */
#define WASMOS_APP_MAX_REQUIRED_ENDPOINTS 8u
#define WASMOS_APP_MAX_CAP_REQUESTS 8u
#define WASMOS_APP_MAX_DRIVER_MATCHES 8u

/* One entry of the endpoint table: the name of an IPC endpoint the package must have
 * resolved before its entry point runs, plus the rights mask it asks for.  `name` is
 * not NUL-terminated and points into the parsed blob; `name_len` bounds it. */
typedef struct {
    const uint8_t* name;
    uint32_t name_len;
    uint32_t rights;
} wasmos_app_req_endpoint_t;

/* One entry of the capability table: a capability name the package requests and the
 * flags it requests it with.  `name` is a non-NUL-terminated alias into the blob. */
typedef struct {
    const uint8_t* name;
    uint32_t name_len;
    uint32_t flags;
} wasmos_app_cap_request_t;

/* One PCI/legacy device pattern a driver package claims.  This layout is also the
 * on-wire record: the parser copies it straight out of the blob, so its size and field
 * order are part of the format.  class_code/subclass/prog_if take
 * WASMOS_DRIVER_MATCH_ANY_U8 and vendor_id/device_id WASMOS_DRIVER_MATCH_ANY_U16 as
 * "don't care".  io_port_min/io_port_max describe a legacy I/O window (inclusive) for
 * non-PCI matches.  `priority` breaks ties when several drivers claim one device; higher
 * wins. */
typedef struct {
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t reserved0;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t io_port_min;
    uint16_t io_port_max;
    uint32_t priority;
} wasmos_app_driver_match_t;

/* Register windows a driver declares it needs, in the order it addresses them.
 * A driver names windows rather than ports: IO is a fixed range it knows
 * statically (legacy/ISA-compat), BAR is wherever firmware put a BAR of the
 * matched device. Declaration order is the region index used at runtime, so the
 * manifest is where a reader learns what "region 1" means. */
/* WASMOS_APP_REGION_IO / _BAR come from wasmos_driver_abi.h, which the kernel
 * also includes: the same two values name the same concept on both sides of the
 * boundary, so there is one definition rather than two that can drift. */

/* Capacity of the region table; a header region_count above this rejects the package. */
#define WASMOS_APP_MAX_REGIONS 4u

/* One declared register window, and also the on-wire record — the parser copies it
 * verbatim out of the blob.  For WASMOS_APP_REGION_IO, [first, last] is an inclusive
 * legacy I/O port range and bar_index is unused; first > last is rejected.  For
 * WASMOS_APP_REGION_BAR, bar_index selects one of the matched device's 6 BARs (>= 6 is
 * rejected) and first/last are unused. */
typedef struct {
    uint8_t kind; /* WASMOS_APP_REGION_* */
    uint8_t bar_index;
    uint16_t first;
    uint16_t last;
} wasmos_app_region_t;

/* Parsed view of a .wap package.  Every pointer field aliases into the `blob` the
 * descriptor was parsed from and is valid only while that blob is live; the fixed-size
 * tables are copies.  Names are length-counted and not NUL-terminated. */
typedef struct {
    const uint8_t* blob;
    uint32_t blob_size;
    uint32_t flags;
    char subsystem_tag[WASMOS_APP_SUBSYSTEM_TAG_LEN + 1];
    const uint8_t* wasm_bytes;
    uint32_t wasm_size;
    const uint8_t* compiled_bytes; /* pre-compiled WARP AOT binary; NULL if absent */
    uint32_t compiled_size;
    const uint8_t* name;
    uint32_t name_len;
    const uint8_t* entry;
    uint32_t entry_len;
    /* min_pages of the STACK / HEAP memory hints, in 4 KiB pages.  0 means the package
     * gave no hint, and wasmos_app_start substitutes 64 KiB. */
    uint32_t stack_pages_hint;
    uint32_t heap_pages_hint;
    uint32_t driver_match_count;
    wasmos_app_driver_match_t driver_matches[WASMOS_APP_MAX_DRIVER_MATCHES];
    uint32_t region_count;
    wasmos_app_region_t regions[WASMOS_APP_MAX_REGIONS];
    uint32_t req_ep_count;
    wasmos_app_req_endpoint_t req_eps[WASMOS_APP_MAX_REQUIRED_ENDPOINTS];
    uint32_t cap_count;
    wasmos_app_cap_request_t caps[WASMOS_APP_MAX_CAP_REQUESTS];
    /* Reserved for future header information.  The .wap format still carries an
     * entry-argument binding section, and the parser still walks it so every
     * following section lands at the right offset, but nothing projects it here:
     * the mechanism it fed -- four spawn arguments delivered in registers -- is
     * inert, since pm_apply_entry_bindings passes zeros and startup values travel
     * in the spawn-info buffer instead.
     *
     * These four words are kept rather than removed so a later header field can
     * claim them without disturbing the struct around them.  Zeroed by
     * wasmos_app_parse; no reader may assume any meaning until one is assigned. */
    uint32_t reserved[4];
} wasmos_app_desc_t;

/* Everything a subsystem's start hook needs, assembled by wasmos_app_start.  All
 * pointers are borrowed for the duration of the call: `name` and `entry_export` point at
 * the instance's own NUL-terminated copies, the byte pointers into the package blob, and
 * `entry_argv` at the instance's argument array.  stack_size and heap_size are in bytes
 * (the descriptor's page hints multiplied by 4096, or 64 KiB when unhinted). */
typedef struct {
    const char* name;
    const uint8_t* module_bytes;
    uint32_t module_size;
    const uint8_t* compiled_bytes;
    uint32_t compiled_size;
    const char* entry_export;
    uint32_t stack_size;
    uint32_t heap_size;
    uint32_t entry_argc;
    const uint32_t* entry_argv;
} wasmos_app_start_params_t;

/* Runtime state of a NATIVE package.  A native payload runs its entry point during
 * start, so entry_rc caches that result and the subsystem's call_entry hook replays it
 * instead of invoking anything. */
typedef struct {
    uint8_t started;
    int32_t entry_rc;
} wasmos_native_instance_t;

/* Runtime state of a started package.  Which arm is live is decided by the resolved
 * subsystem's uses_wasm_payload, not stored here — only the ops that produced it may
 * read it back. */
typedef union {
    wasm_driver_t wasm;
    wasmos_native_instance_t native;
} wasmos_app_runtime_state_t;

/* Result of resolving a package's subsystem tag against the registry: the tag as
 * requested, the tag the running build maps it to, the broker that services it (empty
 * for builtins), and the handler's behaviour flags copied out of the registry entry.
 * `ops` is borrowed from the registry and stays valid as long as the handler stays
 * registered. */
typedef struct {
    char requested_tag[WASMOS_APP_SUBSYSTEM_TAG_LEN + 1];
    char runtime_tag[WASMOS_APP_SUBSYSTEM_TAG_LEN + 1];
    char broker_name[WASMOS_APP_SUBSYSTEM_TAG_LEN + 1];
    wasmos_subsystem_handler_kind_t kind;
    uint8_t uses_wasm_payload;
    uint8_t needs_runtime_lock;
    uint8_t gates_ready_for_services;
    uint32_t broker_endpoint;
    const wasmos_subsystem_ops_t* ops;
} wasmos_app_subsystem_info_t;

/* Vtable a subsystem implementation registers to own the lifecycle of packages carrying
 * its tag.  The three hooks are called only with a non-NULL `state` belonging to the
 * instance, and `start` must leave `state` in a shape its own `call_entry` and `stop`
 * accept.  start/call_entry return 0 on success and non-zero on failure.  The flag fields
 * describe the handler to the process manager: uses_wasm_payload distinguishes a WASM
 * from a native payload (and is cross-checked against WASMOS_APP_FLAG_NATIVE at resolve
 * time), needs_runtime_lock says calls into the runtime must be serialised, and
 * gates_ready_for_services says a service/driver of this kind signals readiness itself
 * rather than being treated as ready once started.  The ops struct is borrowed by the
 * registry, so it must outlive every instance — statics, not stack values. */
typedef struct wasmos_subsystem_ops wasmos_subsystem_ops_t;
struct wasmos_subsystem_ops {
    const char* tag;
    uint8_t uses_wasm_payload;
    uint8_t needs_runtime_lock;
    uint8_t gates_ready_for_services;
    int (*start)(wasmos_app_runtime_state_t* state, const wasmos_app_start_params_t* params,
                 uint32_t owner_context_id, uint32_t flags);
    int (*call_entry)(wasmos_app_runtime_state_t* state, const char* entry_export,
                      uint32_t entry_argc, uint32_t* entry_argv);
    void (*stop)(wasmos_app_runtime_state_t* state);
};

/* Policy hooks wasmos_app_start calls once per endpoint-table / capability-table entry,
 * installed by the process manager via wasmos_app_set_policy_hooks.  `name` is borrowed
 * and not NUL-terminated; `name_len` bounds it.  The resolver must return 0 and write a
 * usable endpoint id to *out_endpoint, or return non-zero; writing IPC_ENDPOINT_NONE is
 * treated as failure either way.  The granter returns 0 when the capability was granted.
 * A non-zero return from either aborts the start with the process partially set up. */
typedef int (*wasmos_app_endpoint_resolver_t)(uint32_t owner_context_id, const uint8_t* name,
                                              uint32_t name_len, uint32_t rights,
                                              uint32_t* out_endpoint);
typedef int (*wasmos_app_capability_granter_t)(uint32_t owner_context_id, const uint8_t* name,
                                               uint32_t name_len, uint32_t flags);

/* A started package.  Unlike wasmos_app_desc_t this owns its data: name and entry are
 * NUL-terminated copies, so the instance outliving the package blob is fine.  `active`
 * is 1 between a successful wasmos_app_start and wasmos_app_stop.  resolved_eps holds
 * the endpoint ids the resolver produced, in endpoint-table order.  entry_argv always
 * holds 4 slots of which the first entry_argc are meaningful. */
typedef struct {
    const wasmos_subsystem_ops_t* ops;
    wasmos_app_runtime_state_t runtime;
    uint8_t active;
    uint32_t flags;
    uint32_t owner_context_id;
    char name[64];
    char entry[64];
    uint32_t resolved_ep_count;
    uint32_t resolved_eps[WASMOS_APP_MAX_REQUIRED_ENDPOINTS];
    uint32_t entry_argc;
    uint32_t entry_argv[4];
} wasmos_app_instance_t;

/* Validate a .wap blob and fill *out_desc with zero-copy views into it.  The parser walks
 * the blob strictly in packer order — header, name, entry, endpoint table, capability
 * table, entry-arg binding table, driver-match table, region table, memory hints, raw
 * payload bytes, then the optional AOT binary — bounds-checking each step with overflow-
 * safe 32-bit arithmetic.  Every header count is walked even when this build ignores the
 * section, because skipping one desynchronises the running offset and misreads
 * everything after it.  Returns 0 on success and -1 for: a NULL argument, a blob shorter
 * than the v1 header, a bad magic, an unsupported version, a header_size that disagrees
 * with the version, a non-zero reserved field, a count above its table capacity, a
 * NATIVE package that is neither driver nor service, an invalid subsystem tag, an
 * inverted I/O window, a BAR index >= 6, or any section that runs past blob_size.
 * `blob` is borrowed: the descriptor's pointers dangle once the blob is freed or moved. */
int wasmos_app_parse(const uint8_t* blob, uint32_t blob_size, wasmos_app_desc_t* out_desc);

/* Register the built-in subsystem handlers (NATIVE, the generic "WASM" tag, and the
 * concrete tag of the runtime this build uses).  Idempotent and internally locked, so
 * repeat calls after the first success are no-ops.  Returns 0 on success, -1 if a
 * registration fails — in which case the initialised flag stays clear and a later call
 * retries. */
int wasmos_app_init_subsystems(void);

/* Add a subsystem handler mapping the package-facing request_tag to runtime_tag, backed
 * by `ops`.  The behaviour flags are taken from `ops`, which is stored by pointer and
 * must outlive every package started through it.  Returns 0 on success, -1 on a NULL ops
 * or a registry rejection (unknown/duplicate tag, table full). */
int wasmos_subsystem_register(const char* request_tag, const char* runtime_tag,
                              const wasmos_subsystem_ops_t* ops);

/* Resolve desc->subsystem_tag through the registry into *out_info.  Returns 0 on
 * success, -1 on a NULL argument, an unregistered tag, or a payload-kind mismatch — a
 * package flagged WASMOS_APP_FLAG_NATIVE must resolve to a handler with
 * uses_wasm_payload == 0 and vice versa. */
int wasmos_app_resolve_subsystem(const wasmos_app_desc_t* desc,
                                 wasmos_app_subsystem_info_t* out_info);

/* Whether the spawner must wait for this package to announce readiness itself rather
 * than treating a successful start as ready.  Returns 1 when it must, 0 when it must not
 * — including for any package that is neither a service nor a driver — and -1 when the
 * descriptor is NULL, its subsystem does not resolve, or the handler is broker-backed
 * rather than built in.  Three-valued: test for 1, not for truthiness. */
int wasmos_app_requires_explicit_ready(const wasmos_app_desc_t* desc);

/* Start `desc` into `instance` on behalf of owner_context_id: copy the name and entry
 * export, resolve every required endpoint and grant every requested capability through
 * the installed policy hooks, resolve the subsystem, and call its start hook.  `init_argv`
 * supplies the entry-point arguments; at most 4 are kept and init_argc is clamped to 4.
 * `desc` and `init_argv` are borrowed for the call only.  Returns 0 with instance->active
 * set, or -1 on a NULL argument, owner_context_id 0, a name or entry that does not fit
 * the instance's 64-byte fields, a missing policy hook, a failed endpoint resolve or
 * capability grant, an unresolved or broker-backed subsystem, or a failing start hook.
 * On failure the instance is left inactive, but capabilities already granted and
 * endpoints already resolved are not rolled back. */
int wasmos_app_start(wasmos_app_instance_t* instance, const wasmos_app_desc_t* desc,
                     uint32_t owner_context_id, const uint32_t* init_argv, uint32_t init_argc);

/* Invoke the started instance's entry export through its subsystem's call_entry hook,
 * with the arguments captured at start.  Returns the hook's status (0 = success), or -1
 * when the instance is NULL, inactive, or has no call_entry hook.  Blocks for as long as
 * the guest entry point runs. */
int wasmos_app_call_entry(wasmos_app_instance_t* instance);

/* Stop a started instance through its subsystem's stop hook and clear its runtime
 * bookkeeping.  Idempotent: a NULL or already-inactive instance is ignored.  Does not
 * release the endpoints or capabilities wasmos_app_start acquired. */
void wasmos_app_stop(wasmos_app_instance_t* instance);

/* Install the process-wide endpoint resolver and capability granter wasmos_app_start
 * uses.  Either may be NULL, which makes a package that requires that kind of entry fail
 * to start.  Last call wins; the hooks are global, not per instance. */
void wasmos_app_set_policy_hooks(wasmos_app_endpoint_resolver_t endpoint_resolver,
                                 wasmos_app_capability_granter_t capability_granter);

#endif
