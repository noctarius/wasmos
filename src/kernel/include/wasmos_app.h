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
 * and handles capability granting via an injected callback. */
#ifndef WASMOS_APP_H
#define WASMOS_APP_H

#include <stdint.h>
#include "boot.h"
#include "ipc.h"
#include "subsystem_registry.h"
#include "wasm_driver.h"

/* First 8 bytes of every .wap blob, compared byte for byte (not NUL-terminated on the
 * wire). */
#define WASMOS_APP_MAGIC "WASMOSAP"
/* Container version the packer (scripts/make_wasmos_app.c) emits, and the only one
 * the parser accepts.  The version is bound to an exact header_size, so a blob whose
 * header_size disagrees is rejected rather than reinterpreted. */
#define WASMOS_APP_VERSION 12u
/* Most WARP AOT binaries one package carries: one per architecture it ships
 * compiled code for (v12). */
#define WASMOS_APP_MAX_COMPILED 4u
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

/* `kind` values of a memory-hint record in the blob.  Only STACK and HEAP are consumed
 * by the parser (as min_pages); the others are parsed for section walking and ignored. */
#define WASMOS_APP_MEM_HINT_LINEAR 0u
#define WASMOS_APP_MEM_HINT_STACK 1u
#define WASMOS_APP_MEM_HINT_HEAP 2u
#define WASMOS_APP_MEM_HINT_IPC 3u
#define WASMOS_APP_MEM_HINT_DEVICE 4u

/* Byte length of a package signature: Ed25519 (RFC 8032), over every byte of the
 * package before it. */
#define WASMOS_APP_SIGNATURE_LEN 64u

/* Capacities of the fixed-size tables in wasmos_app_desc_t.  A header count above any of
 * these makes wasmos_app_parse reject the whole package rather than truncate — a
 * truncated table would desynchronise the section walk. */
#define WASMOS_APP_MAX_DEPS 16u
#define WASMOS_APP_MAX_CAP_REQUESTS 8u

/* Dependency declarations (the manifest's [deps] table, v10+).  A record names a
 * service by NAME (as registered with the process manager) or a service CLASS, never a
 * binary.  `strength` says what the package does with it:
 *   NEEDS     it cannot do its job without one; a supervisor orders shutdown and
 *             restarts by it, and the build rejects a cycle of NEEDS edges;
 *   WANTS     it uses one when present and degrades without it;
 *   PROVIDES  it registers that name or joins that class itself.
 * The records are data only: neither the parser nor the process manager gates a
 * start on them -- a package follows its peers at run time (service-name watches).
 * Name lengths are bounded by the registry's limits: WASMOS_APP_DEP_NAME_MAX for a
 * name, WASMOS_APP_DEP_CLASS_MAX for a class (both excluding a terminator). */
#define WASMOS_APP_DEP_NEEDS 1u
#define WASMOS_APP_DEP_WANTS 2u
#define WASMOS_APP_DEP_PROVIDES 3u
#define WASMOS_APP_DEP_KIND_NAME 1u
#define WASMOS_APP_DEP_KIND_CLASS 2u
#define WASMOS_APP_DEP_NAME_MAX 35u  /* WASMOS_SVC_NAME_MAX - 1 */
#define WASMOS_APP_DEP_CLASS_MAX 15u /* WASMOS_SVC_CLASS_MAX - 1 */

/* One parsed dependency record.  `name` is not NUL-terminated and points into the
 * parsed blob; `name_len` bounds it. */
typedef struct {
    uint8_t strength; /* WASMOS_APP_DEP_NEEDS / _WANTS / _PROVIDES */
    uint8_t kind;     /* WASMOS_APP_DEP_KIND_NAME / _CLASS */
    const uint8_t* name;
    uint32_t name_len;
} wasmos_app_dep_t;

/* One entry of the capability table: a capability name the package requests and the
 * flags it requests it with.  `name` is a non-NUL-terminated alias into the blob. */
typedef struct {
    const uint8_t* name;
    uint32_t name_len;
    uint32_t flags;
} wasmos_app_cap_request_t;

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
    /* The package's WARP AOT binaries, one per architecture, each tagged with the
     * ELF e_machine of the code it holds (EM_X86_64, EM_AARCH64).  WARP records no
     * ISA in a compiled binary, so the tag is the only thing that keeps a binary
     * for one architecture from executing on another; wasmos_app_compiled_for
     * selects by it.  The parser guarantees non-zero sizes and distinct tags. */
    uint32_t compiled_count;
    struct {
        uint16_t machine;
        const uint8_t* bytes;
        uint32_t size;
    } compiled[WASMOS_APP_MAX_COMPILED];
    /* The package's Ed25519 signature (WASMOS_APP_SIGNATURE_LEN bytes, the last in
     * the blob) and the length of what it signs: every byte before it.  NULL and 0
     * for an unsigned package.  Parsing locates the signature; whether it is valid
     * is wasmos_app_signature_valid's question. */
    const uint8_t* signature;
    uint32_t signed_size;
    const uint8_t* name;
    uint32_t name_len;
    const uint8_t* entry;
    uint32_t entry_len;
    /* min_pages of the STACK / HEAP memory hints, in 4 KiB pages.  0 means the package
     * gave no hint, and wasmos_app_start substitutes 64 KiB. */
    uint32_t stack_pages_hint;
    uint32_t heap_pages_hint;
    uint32_t region_count;
    wasmos_app_region_t regions[WASMOS_APP_MAX_REGIONS];
    uint32_t dep_count;
    wasmos_app_dep_t deps[WASMOS_APP_MAX_DEPS];
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

/* What decides a manifest capability beyond its name, passed to the granter with
 * each one: whether the package is signed with the build's key
 * (wasmos_app_signature_valid), and what the process that requested the spawn
 * held when it asked (capability_mask_of; every bit for a spawn the kernel made
 * itself).  `image_name` is the package's name, borrowed and not NUL-terminated,
 * for the granter's log line. */
typedef struct {
    uint8_t image_signed;
    uint32_t requester_cap_mask;
    const uint8_t* image_name;
    uint32_t image_name_len;
} wasmos_app_grant_ctx_t;

/* Policy hook wasmos_app_start calls once per capability-table entry, installed by the
 * process manager via wasmos_app_set_policy_hooks.  `name` is borrowed and not
 * NUL-terminated; `name_len` bounds it.  Returns 0 when the capability was granted; a
 * non-zero return aborts the start with the process partially set up. */
typedef int (*wasmos_app_capability_granter_t)(uint32_t owner_context_id, const uint8_t* name,
                                               uint32_t name_len, uint32_t flags,
                                               const wasmos_app_grant_ctx_t* ctx);

/* 1 when the package carries a signature (wasmos_app_desc_t.signature) that is a valid
 * Ed25519 signature, by the key compiled into this kernel
 * (wasmos_image_signing_pubkey), over every byte before it; 0 otherwise, including for
 * an unsigned package.  Hashes the whole package, so it costs time proportional to
 * its size. */
int wasmos_app_signature_valid(const wasmos_app_desc_t* desc);

/* A started package.  Unlike wasmos_app_desc_t this owns its data: name and entry are
 * NUL-terminated copies, so the instance outliving the package blob is fine.  `active`
 * is 1 between a successful wasmos_app_start and wasmos_app_stop.  entry_argv always
 * holds 4 slots of which the first entry_argc are meaningful. */
typedef struct {
    const wasmos_subsystem_ops_t* ops;
    wasmos_app_runtime_state_t runtime;
    uint8_t active;
    uint32_t flags;
    uint32_t owner_context_id;
    char name[64];
    char entry[64];
    uint32_t entry_argc;
    uint32_t entry_argv[4];
} wasmos_app_instance_t;

/* Validate a .wap blob and fill *out_desc with zero-copy views into it.  The parser walks
 * the blob strictly in packer order — header, name, entry, dependency table, capability
 * table, region table, memory hints, raw payload bytes, then the AOT table and binaries —
 * bounds-checking each step with overflow-
 * safe 32-bit arithmetic.  Every header count is walked even when this build ignores the
 * section, because skipping one desynchronises the running offset and misreads
 * everything after it.  Returns 0 on success and -1 for: a NULL argument, a blob shorter
 * than the v1 header, a bad magic, an unsupported version, a header_size that disagrees
 * with the version, a count above its table capacity, a dependency record with an
 * unknown strength or kind or a name that is empty or over its kind's limit, a
 * NATIVE package that is neither driver nor service, an invalid subsystem tag, an
 * inverted I/O window, a BAR index >= 6, an AOT entry with a zero machine or size
 * or a machine already listed, or any section that runs past blob_size.
 * `blob` is borrowed: the descriptor's pointers dangle once the blob is freed or moved. */
int wasmos_app_parse(const uint8_t* blob, uint32_t blob_size, wasmos_app_desc_t* out_desc);

/* The package's AOT binary for ELF machine `machine` (ARCH_ELF_MACHINE for this
 * kernel): 0 with *bytes and *size set when the package carries one, -1 with
 * both cleared when it does not.  A package without one for this architecture is
 * still runnable from its bytecode. */
int wasmos_app_compiled_for(const wasmos_app_desc_t* desc, uint16_t machine, const uint8_t** bytes,
                            uint32_t* size);

/* Index into info's boot module table of the WASMOS-APP whose embedded app name
 * is the name_len bytes at `name`, or 0xFFFFFFFF when the table holds none (or
 * info carries no module table). A module that does not parse is skipped. The
 * first match wins; app names in one boot image are unique by construction. */
uint32_t wasmos_app_boot_module_index(const boot_info_t* info, const char* name, uint32_t name_len);

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
 * export, grant every requested capability through the installed policy hook, resolve
 * the subsystem, and call its start hook.  `init_argv`
 * supplies the entry-point arguments; at most 4 are kept and init_argc is clamped to 4.
 * `desc` and `init_argv` are borrowed for the call only.  Returns 0 with instance->active
 * set, or -1 on a NULL argument, owner_context_id 0, a name or entry that does not fit
 * the instance's 64-byte fields, a missing policy hook, a refused capability grant, an
 * unresolved or broker-backed subsystem, or a failing start hook.  The dependency table
 * is not consulted.  On failure the instance is left inactive, but capabilities already
 * granted are not rolled back.
 *
 * requester_cap_mask is what the process that asked for this spawn held when it asked
 * (see wasmos_app_grant_ctx_t); the policy hook decides each capability with it and
 * with whether the package's signature is valid, which is checked here only when the
 * package asks for a capability other than ipc.basic. */
int wasmos_app_start(wasmos_app_instance_t* instance, const wasmos_app_desc_t* desc,
                     uint32_t owner_context_id, uint32_t requester_cap_mask,
                     const uint32_t* init_argv, uint32_t init_argc);

/* Invoke the started instance's entry export through its subsystem's call_entry hook,
 * with the arguments captured at start.  Returns the hook's status (0 = success), or -1
 * when the instance is NULL, inactive, or has no call_entry hook.  Blocks for as long as
 * the guest entry point runs. */
int wasmos_app_call_entry(wasmos_app_instance_t* instance);

/* Stop a started instance through its subsystem's stop hook and clear its runtime
 * bookkeeping.  Idempotent: a NULL or already-inactive instance is ignored.  Does not
 * release the capabilities wasmos_app_start granted. */
void wasmos_app_stop(wasmos_app_instance_t* instance);

/* Install the process-wide capability granter wasmos_app_start uses.  NULL makes a
 * package that requests any capability fail to start.  Last call wins; the hook is
 * global, not per instance. */
void wasmos_app_set_policy_hooks(wasmos_app_capability_granter_t capability_granter);

#endif
