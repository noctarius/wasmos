/* wasmos_app.c - WASMOS-APP (.wap) package parser and instance launcher.
 * wasmos_app_parse() validates the magic/version and builds a wasmos_app_desc_t
 * with zero-copy pointers into the blob.  wasmos_app_start() creates the WASM
 * driver instance, resolves endpoints, grants capabilities, and prepares entry args. */
#include "klog.h"
#include "native_driver.h"
#include "serial.h"
#include "sync/spinlock.h"
#include "subsystem_registry.h"
#include "wasmos_app.h"
#include <string.h>

/*
 * WASMOS-APP parsing is kept separate from process-manager policy so the binary
 * format stays testable and reusable. This file validates container structure,
 * exposes parsed descriptors, and translates metadata into runtime startup.
 */

typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t flags;
    uint32_t name_len;
    uint32_t entry_len;
    uint32_t wasm_size;
    uint32_t req_ep_count;
    uint32_t cap_count;
    uint32_t entry_arg_binding_count;
    uint32_t mem_hint_count;
    uint8_t driver_match_class;
    uint8_t driver_match_subclass;
    uint8_t driver_match_prog_if;
    uint8_t driver_match_reserved0;
    uint16_t driver_match_vendor_id;
    uint16_t driver_match_device_id;
    uint16_t driver_io_port_min;
    uint16_t driver_io_port_max;
    uint32_t reserved;
} wasmos_app_header_v2_t;

typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t flags;
    uint32_t name_len;
    uint32_t entry_len;
    uint32_t wasm_size;
    uint32_t req_ep_count;
    uint32_t cap_count;
    uint32_t entry_arg_binding_count;
    uint32_t mem_hint_count;
    uint8_t driver_match_class;
    uint8_t driver_match_subclass;
    uint8_t driver_match_prog_if;
    uint8_t driver_match_reserved0;
    uint16_t driver_match_vendor_id;
    uint16_t driver_match_device_id;
    uint16_t driver_io_port_min;
    uint16_t driver_io_port_max;
    uint32_t driver_match_count;
    uint32_t reserved;
} wasmos_app_header_v3_t;

/* Version 4: identical to v3 except reserved is repurposed as compiled_size.
 * When compiled_size > 0, a pre-compiled WARP AOT binary follows immediately
 * after the WASM bytes in the blob. */
typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t flags;
    uint32_t name_len;
    uint32_t entry_len;
    uint32_t wasm_size;
    uint32_t req_ep_count;
    uint32_t cap_count;
    uint32_t entry_arg_binding_count;
    uint32_t mem_hint_count;
    uint8_t driver_match_class;
    uint8_t driver_match_subclass;
    uint8_t driver_match_prog_if;
    uint8_t driver_match_reserved0;
    uint16_t driver_match_vendor_id;
    uint16_t driver_match_device_id;
    uint16_t driver_io_port_min;
    uint16_t driver_io_port_max;
    uint32_t driver_match_count;
    uint32_t compiled_size; /* size of WARP AOT binary appended after WASM bytes; 0 if absent */
} wasmos_app_header_v4_t;

typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t flags;
    uint32_t name_len;
    uint32_t entry_len;
    uint32_t wasm_size;
    uint32_t req_ep_count;
    uint32_t cap_count;
    uint32_t entry_arg_binding_count;
    uint32_t mem_hint_count;
    uint8_t driver_match_class;
    uint8_t driver_match_subclass;
    uint8_t driver_match_prog_if;
    uint8_t driver_match_reserved0;
    uint16_t driver_match_vendor_id;
    uint16_t driver_match_device_id;
    uint16_t driver_io_port_min;
    uint16_t driver_io_port_max;
    uint32_t driver_match_count;
    uint32_t compiled_size;
    char subsystem_tag[WASMOS_APP_SUBSYSTEM_TAG_LEN];
} wasmos_app_header_v5_t;

/* Version 6: v5 plus the declared register windows, written after the driver
 * matches. A driver names the windows it needs instead of the ports, so this is
 * where "region 1" acquires a meaning. */
typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t flags;
    uint32_t name_len;
    uint32_t entry_len;
    uint32_t wasm_size;
    uint32_t req_ep_count;
    uint32_t cap_count;
    uint32_t entry_arg_binding_count;
    uint32_t mem_hint_count;
    uint8_t driver_match_class;
    uint8_t driver_match_subclass;
    uint8_t driver_match_prog_if;
    uint8_t driver_match_reserved0;
    uint16_t driver_match_vendor_id;
    uint16_t driver_match_device_id;
    uint16_t driver_io_port_min;
    uint16_t driver_io_port_max;
    uint32_t driver_match_count;
    uint32_t compiled_size;
    char subsystem_tag[WASMOS_APP_SUBSYSTEM_TAG_LEN];
    uint32_t region_count;
} wasmos_app_header_v6_t;

typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t flags;
    uint32_t name_len;
    uint32_t entry_len;
    uint32_t wasm_size;
    uint32_t req_ep_count;
    uint32_t cap_count;
    uint32_t mem_hint_count;
    uint32_t reserved;
} wasmos_app_header_v1_t;

typedef struct __attribute__((packed)) {
    uint32_t name_len;
    uint32_t rights;
} wasmos_req_endpoint_t;

typedef struct __attribute__((packed)) {
    uint32_t name_len;
    uint32_t flags;
} wasmos_cap_request_t;

typedef struct __attribute__((packed)) {
    uint32_t name_len;
} wasmos_entry_arg_binding_t;

typedef struct __attribute__((packed)) {
    uint32_t kind;
    uint32_t min_pages;
    uint32_t max_pages;
} wasmos_mem_hint_t;

static wasmos_app_endpoint_resolver_t g_endpoint_resolver;
static wasmos_app_capability_granter_t g_capability_granter;
static uint8_t g_subsystems_initialized;
static ksync_spinlock_t g_subsystem_lock;
static uint8_t g_subsystem_lock_initialized;

#if WASMOS_WASM_RUNTIME == 1
#define WASMOS_ACTIVE_WASM_SUBSYSTEM_TAG WASMOS_SUBSYSTEM_TAG_WARP
#else
#define WASMOS_ACTIVE_WASM_SUBSYSTEM_TAG WASMOS_SUBSYSTEM_TAG_WASM3
#endif

static int wasmos_wasm_subsystem_start(wasmos_app_runtime_state_t* state,
                                       const wasmos_app_start_params_t* params,
                                       uint32_t owner_context_id, uint32_t flags) {
    wasm_driver_manifest_t manifest;
    (void)flags;
    if (!state || !params) {
        return -1;
    }
    manifest.name = params->name;
    manifest.module_bytes = params->module_bytes;
    manifest.module_size = params->module_size;
    manifest.compiled_bytes = params->compiled_bytes;
    manifest.compiled_size = params->compiled_size;
    manifest.entry_export = 0;
    manifest.entry_argc = 0;
    manifest.entry_argv = 0;
    manifest.stack_size = params->stack_size;
    manifest.heap_size = params->heap_size;
    return wasm_driver_start(&state->wasm, &manifest, owner_context_id);
}

static int wasmos_wasm_subsystem_call_entry(wasmos_app_runtime_state_t* state,
                                            const char* entry_export, uint32_t entry_argc,
                                            uint32_t* entry_argv) {
    if (!state) {
        return -1;
    }
    return wasm_driver_call_unlocked(&state->wasm, entry_export, entry_argc, entry_argv);
}

static void wasmos_wasm_subsystem_stop(wasmos_app_runtime_state_t* state) {
    if (!state) {
        return;
    }
    wasm_driver_stop(&state->wasm);
}

static int wasmos_native_subsystem_start(wasmos_app_runtime_state_t* state,
                                         const wasmos_app_start_params_t* params,
                                         uint32_t owner_context_id, uint32_t flags) {
    int rc = -1;
    (void)flags;
    if (!state || !params) {
        return -1;
    }
    rc = native_driver_start(owner_context_id, params->module_bytes, params->module_size,
                             params->name, params->entry_argv, params->entry_argc);
    state->native.started = 1;
    state->native.entry_rc = rc;
    return rc == 0 ? 0 : -1;
}

static int wasmos_native_subsystem_call_entry(wasmos_app_runtime_state_t* state,
                                              const char* entry_export, uint32_t entry_argc,
                                              uint32_t* entry_argv) {
    (void)entry_export;
    (void)entry_argc;
    (void)entry_argv;
    if (!state || !state->native.started) {
        return -1;
    }
    return state->native.entry_rc;
}

static void wasmos_native_subsystem_stop(wasmos_app_runtime_state_t* state) {
    if (!state) {
        return;
    }
    state->native.started = 0;
    state->native.entry_rc = 0;
}

static const wasmos_subsystem_ops_t g_wasmos_wasm_subsystem_ops = {
    .tag = WASMOS_ACTIVE_WASM_SUBSYSTEM_TAG,
    .uses_wasm_payload = 1u,
    .needs_runtime_lock = 1u,
    .gates_ready_for_services = 1u,
    .start = wasmos_wasm_subsystem_start,
    .call_entry = wasmos_wasm_subsystem_call_entry,
    .stop = wasmos_wasm_subsystem_stop,
};

static const wasmos_subsystem_ops_t g_wasmos_native_subsystem_ops = {
    .tag = WASMOS_SUBSYSTEM_TAG_NATIVE,
    .uses_wasm_payload = 0u,
    .needs_runtime_lock = 0u,
    .gates_ready_for_services = 1u,
    .start = wasmos_native_subsystem_start,
    .call_entry = wasmos_native_subsystem_call_entry,
    .stop = wasmos_native_subsystem_stop,
};

static void copy_subsystem_tag(char* dst, const char* src) {
    if (!dst) {
        return;
    }
    /* Zero-fill the whole field, then truncate-copy the tag into it. */
    memset(dst, 0, WASMOS_APP_SUBSYSTEM_TAG_LEN + 1);
    (void)str_copy(dst, WASMOS_APP_SUBSYSTEM_TAG_LEN + 1, src);
}

static int subsystem_tag_has_valid_char(char c) {
    return ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) || c == '+' || c == '_' ||
           c == '-';
}

static int subsystem_tag_validate_bytes(const char* tag, uint32_t len) {
    int saw_nul = 0;
    if (!tag || len == 0) {
        return -1;
    }
    for (uint32_t i = 0; i < len; ++i) {
        char c = tag[i];
        if (c == '\0') {
            saw_nul = 1;
            continue;
        }
        if (saw_nul || !subsystem_tag_has_valid_char(c)) {
            return -1;
        }
    }
    return tag[0] == '\0' ? -1 : 0;
}

static void subsystem_tag_default_for_flags(uint32_t flags,
                                            char out_tag[WASMOS_APP_SUBSYSTEM_TAG_LEN + 1]) {
    copy_subsystem_tag(out_tag, (flags & WASMOS_APP_FLAG_NATIVE) != 0 ? WASMOS_SUBSYSTEM_TAG_NATIVE
                                                                      : WASMOS_SUBSYSTEM_TAG_WASM);
}

static int check_u32_add(uint32_t a, uint32_t b, uint32_t* out) {
    uint64_t sum = (uint64_t)a + (uint64_t)b;
    if (sum > 0xFFFFFFFFULL) {
        return -1;
    }
    *out = (uint32_t)sum;
    return 0;
}

static int check_bounds(uint32_t offset, uint32_t size, uint32_t blob_size) {
    /* All variable-sized sections are bounds-checked with 32-bit arithmetic
     * overflow protection before any pointer arithmetic is trusted. */
    uint32_t end = 0;
    if (check_u32_add(offset, size, &end) != 0) {
        return -1;
    }
    if (end > blob_size) {
        return -1;
    }
    return 0;
}

static void wasmos_subsystem_lock_init_once(void) {
    if (!g_subsystem_lock_initialized) {
        ksync_spinlock_init(&g_subsystem_lock);
        g_subsystem_lock_initialized = 1u;
    }
}

static int wasmos_subsystem_register_locked(const char* request_tag, const char* runtime_tag,
                                            const wasmos_subsystem_ops_t* ops) {
    if (!ops) {
        return -1;
    }
    return wasmos_subsystem_registry_register_builtin(
        request_tag, runtime_tag, ops->uses_wasm_payload, ops->needs_runtime_lock,
        ops->gates_ready_for_services, ops);
}

int wasmos_subsystem_register(const char* request_tag, const char* runtime_tag,
                              const wasmos_subsystem_ops_t* ops) {
    int rc = -1;
    wasmos_subsystem_lock_init_once();
    ksync_spinlock_lock(&g_subsystem_lock);
    rc = wasmos_subsystem_register_locked(request_tag, runtime_tag, ops);
    ksync_spinlock_unlock(&g_subsystem_lock);
    return rc;
}

static int wasmos_register_builtin_subsystems(void) {
    if (wasmos_subsystem_register_locked(WASMOS_SUBSYSTEM_TAG_NATIVE, WASMOS_SUBSYSTEM_TAG_NATIVE,
                                         &g_wasmos_native_subsystem_ops) != 0 ||
        wasmos_subsystem_register_locked(WASMOS_SUBSYSTEM_TAG_WASM,
                                         WASMOS_ACTIVE_WASM_SUBSYSTEM_TAG,
                                         &g_wasmos_wasm_subsystem_ops) != 0 ||
        wasmos_subsystem_register_locked(WASMOS_ACTIVE_WASM_SUBSYSTEM_TAG,
                                         WASMOS_ACTIVE_WASM_SUBSYSTEM_TAG,
                                         &g_wasmos_wasm_subsystem_ops) != 0) {
        return -1;
    }
#if WASMOS_WASM_RUNTIME == 1
    if (wasmos_subsystem_register_locked("WARP+JIT", WASMOS_ACTIVE_WASM_SUBSYSTEM_TAG,
                                         &g_wasmos_wasm_subsystem_ops) != 0) {
        return -1;
    }
#endif
    return 0;
}

static int wasmos_register_builtin_subsystems_once_locked(void) {
    if (g_subsystems_initialized) {
        return 0;
    }
    if (wasmos_register_builtin_subsystems() != 0) {
        return -1;
    }
    g_subsystems_initialized = 1u;
    return 0;
}

int wasmos_app_init_subsystems(void) {
    int rc = -1;
    wasmos_subsystem_lock_init_once();
    ksync_spinlock_lock(&g_subsystem_lock);
    rc = wasmos_register_builtin_subsystems_once_locked();
    ksync_spinlock_unlock(&g_subsystem_lock);
    return rc;
}

static const wasmos_subsystem_registry_entry_t*
wasmos_app_find_subsystem_handler(const char* request_tag) {
    if (!request_tag) {
        return 0;
    }
    return wasmos_subsystem_registry_find(request_tag);
}

int wasmos_app_parse(const uint8_t* blob, uint32_t blob_size, wasmos_app_desc_t* out_desc) {
    if (!blob || !out_desc || blob_size < sizeof(wasmos_app_header_v1_t)) {
        return -1;
    }
    const wasmos_app_header_v1_t* hdr_v1 = (const wasmos_app_header_v1_t*)blob;
    uint32_t version = hdr_v1->version;
    uint32_t header_size = 0;
    uint32_t flags = 0;
    uint32_t name_len = 0;
    uint32_t entry_len = 0;
    uint32_t wasm_size = 0;
    uint32_t req_ep_count = 0;
    uint32_t cap_count = 0;
    uint32_t entry_arg_binding_count = 0;
    uint32_t mem_hint_count = 0;
    uint32_t reserved = 0;
    uint32_t compiled_size = 0;
    char subsystem_tag[WASMOS_APP_SUBSYSTEM_TAG_LEN + 1];
    wasmos_app_driver_match_t driver_matches[WASMOS_APP_MAX_DRIVER_MATCHES];
    subsystem_tag_default_for_flags(0, subsystem_tag);
    for (uint32_t i = 0; i < WASMOS_APP_MAX_DRIVER_MATCHES; ++i) {
        driver_matches[i].class_code = WASMOS_DRIVER_MATCH_ANY_U8;
        driver_matches[i].subclass = WASMOS_DRIVER_MATCH_ANY_U8;
        driver_matches[i].prog_if = WASMOS_DRIVER_MATCH_ANY_U8;
        driver_matches[i].reserved0 = 0;
        driver_matches[i].vendor_id = WASMOS_DRIVER_MATCH_ANY_U16;
        driver_matches[i].device_id = WASMOS_DRIVER_MATCH_ANY_U16;
        driver_matches[i].io_port_min = 0;
        driver_matches[i].io_port_max = 0;
        driver_matches[i].priority = 0;
    }
    uint32_t driver_match_count = 0;
    uint32_t region_count = 0;
    if (version == 1u) {
        if (blob_size < sizeof(wasmos_app_header_v1_t)) {
            return -1;
        }
        header_size = hdr_v1->header_size;
        flags = hdr_v1->flags;
        name_len = hdr_v1->name_len;
        entry_len = hdr_v1->entry_len;
        wasm_size = hdr_v1->wasm_size;
        req_ep_count = hdr_v1->req_ep_count;
        cap_count = hdr_v1->cap_count;
        mem_hint_count = hdr_v1->mem_hint_count;
        reserved = hdr_v1->reserved;
    } else if (version == 2u) {
        if (blob_size < sizeof(wasmos_app_header_v2_t)) {
            return -1;
        }
        const wasmos_app_header_v2_t* hdr_v2 = (const wasmos_app_header_v2_t*)blob;
        header_size = hdr_v2->header_size;
        flags = hdr_v2->flags;
        name_len = hdr_v2->name_len;
        entry_len = hdr_v2->entry_len;
        wasm_size = hdr_v2->wasm_size;
        req_ep_count = hdr_v2->req_ep_count;
        cap_count = hdr_v2->cap_count;
        entry_arg_binding_count = hdr_v2->entry_arg_binding_count;
        mem_hint_count = hdr_v2->mem_hint_count;
        reserved = hdr_v2->reserved;
        if (hdr_v2->driver_match_class != WASMOS_DRIVER_MATCH_ANY_U8 ||
            hdr_v2->driver_match_subclass != WASMOS_DRIVER_MATCH_ANY_U8 ||
            hdr_v2->driver_match_prog_if != WASMOS_DRIVER_MATCH_ANY_U8 ||
            hdr_v2->driver_match_vendor_id != WASMOS_DRIVER_MATCH_ANY_U16 ||
            hdr_v2->driver_match_device_id != WASMOS_DRIVER_MATCH_ANY_U16) {
            driver_match_count = 1;
            driver_matches[0].class_code = hdr_v2->driver_match_class;
            driver_matches[0].subclass = hdr_v2->driver_match_subclass;
            driver_matches[0].prog_if = hdr_v2->driver_match_prog_if;
            driver_matches[0].reserved0 = 0;
            driver_matches[0].vendor_id = hdr_v2->driver_match_vendor_id;
            driver_matches[0].device_id = hdr_v2->driver_match_device_id;
            driver_matches[0].io_port_min = hdr_v2->driver_io_port_min;
            driver_matches[0].io_port_max = hdr_v2->driver_io_port_max;
            driver_matches[0].priority = 0;
        }
    } else if (version == 3u) {
        if (blob_size < sizeof(wasmos_app_header_v3_t)) {
            return -1;
        }
        const wasmos_app_header_v3_t* hdr_v3 = (const wasmos_app_header_v3_t*)blob;
        header_size = hdr_v3->header_size;
        flags = hdr_v3->flags;
        name_len = hdr_v3->name_len;
        entry_len = hdr_v3->entry_len;
        wasm_size = hdr_v3->wasm_size;
        req_ep_count = hdr_v3->req_ep_count;
        cap_count = hdr_v3->cap_count;
        entry_arg_binding_count = hdr_v3->entry_arg_binding_count;
        mem_hint_count = hdr_v3->mem_hint_count;
        driver_match_count = hdr_v3->driver_match_count;
        if (driver_match_count > WASMOS_APP_MAX_DRIVER_MATCHES) {
            return -1;
        }
        reserved = hdr_v3->reserved;
    } else if (version == 4u) {
        if (blob_size < sizeof(wasmos_app_header_v4_t)) {
            return -1;
        }
        const wasmos_app_header_v4_t* hdr_v4 = (const wasmos_app_header_v4_t*)blob;
        header_size = hdr_v4->header_size;
        flags = hdr_v4->flags;
        name_len = hdr_v4->name_len;
        entry_len = hdr_v4->entry_len;
        wasm_size = hdr_v4->wasm_size;
        req_ep_count = hdr_v4->req_ep_count;
        cap_count = hdr_v4->cap_count;
        entry_arg_binding_count = hdr_v4->entry_arg_binding_count;
        mem_hint_count = hdr_v4->mem_hint_count;
        driver_match_count = hdr_v4->driver_match_count;
        if (driver_match_count > WASMOS_APP_MAX_DRIVER_MATCHES) {
            return -1;
        }
        compiled_size = hdr_v4->compiled_size;
        reserved = 0; /* v4 has no reserved field */
        subsystem_tag_default_for_flags(flags, subsystem_tag);
    } else if (version == 5u) {
        if (blob_size < sizeof(wasmos_app_header_v5_t)) {
            return -1;
        }
        const wasmos_app_header_v5_t* hdr_v5 = (const wasmos_app_header_v5_t*)blob;
        header_size = hdr_v5->header_size;
        flags = hdr_v5->flags;
        name_len = hdr_v5->name_len;
        entry_len = hdr_v5->entry_len;
        wasm_size = hdr_v5->wasm_size;
        req_ep_count = hdr_v5->req_ep_count;
        cap_count = hdr_v5->cap_count;
        entry_arg_binding_count = hdr_v5->entry_arg_binding_count;
        mem_hint_count = hdr_v5->mem_hint_count;
        driver_match_count = hdr_v5->driver_match_count;
        if (driver_match_count > WASMOS_APP_MAX_DRIVER_MATCHES) {
            return -1;
        }
        compiled_size = hdr_v5->compiled_size;
        reserved = 0;
        for (uint32_t i = 0; i < WASMOS_APP_SUBSYSTEM_TAG_LEN; ++i) {
            subsystem_tag[i] = hdr_v5->subsystem_tag[i];
        }
        subsystem_tag[WASMOS_APP_SUBSYSTEM_TAG_LEN] = '\0';
    } else if (version == WASMOS_APP_VERSION) {
        if (blob_size < sizeof(wasmos_app_header_v6_t)) {
            return -1;
        }
        const wasmos_app_header_v6_t* hdr_v6 = (const wasmos_app_header_v6_t*)blob;
        header_size = hdr_v6->header_size;
        flags = hdr_v6->flags;
        name_len = hdr_v6->name_len;
        entry_len = hdr_v6->entry_len;
        wasm_size = hdr_v6->wasm_size;
        req_ep_count = hdr_v6->req_ep_count;
        cap_count = hdr_v6->cap_count;
        entry_arg_binding_count = hdr_v6->entry_arg_binding_count;
        mem_hint_count = hdr_v6->mem_hint_count;
        driver_match_count = hdr_v6->driver_match_count;
        if (driver_match_count > WASMOS_APP_MAX_DRIVER_MATCHES) {
            return -1;
        }
        region_count = hdr_v6->region_count;
        if (region_count > WASMOS_APP_MAX_REGIONS) {
            return -1;
        }
        compiled_size = hdr_v6->compiled_size;
        reserved = 0;
        for (uint32_t i = 0; i < WASMOS_APP_SUBSYSTEM_TAG_LEN; ++i) {
            subsystem_tag[i] = hdr_v6->subsystem_tag[i];
        }
        subsystem_tag[WASMOS_APP_SUBSYSTEM_TAG_LEN] = '\0';
    } else {
        return -1;
    }
    if ((version == 1u && header_size != sizeof(wasmos_app_header_v1_t)) ||
        (version == 2u && header_size != sizeof(wasmos_app_header_v2_t)) ||
        (version == 3u && header_size != sizeof(wasmos_app_header_v3_t)) ||
        (version == 4u && header_size != sizeof(wasmos_app_header_v4_t)) ||
        (version == 5u && header_size != sizeof(wasmos_app_header_v5_t)) ||
        (version == WASMOS_APP_VERSION && header_size != sizeof(wasmos_app_header_v6_t)) ||
        reserved != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < 8; ++i) {
        if ((uint8_t)hdr_v1->magic[i] != (uint8_t)WASMOS_APP_MAGIC[i]) {
            return -1;
        }
    }
    if (req_ep_count > WASMOS_APP_MAX_REQUIRED_ENDPOINTS ||
        cap_count > WASMOS_APP_MAX_CAP_REQUESTS ||
        entry_arg_binding_count > WASMOS_APP_MAX_ENTRY_ARG_BINDINGS) {
        return -1;
    }
    /* Native payloads are privileged and valid for driver/service kinds. */
    if ((flags & WASMOS_APP_FLAG_NATIVE) &&
        !(flags & (WASMOS_APP_FLAG_DRIVER | WASMOS_APP_FLAG_SERVICE))) {
        return -1;
    }
    if (subsystem_tag_validate_bytes(subsystem_tag, WASMOS_APP_SUBSYSTEM_TAG_LEN) != 0) {
        return -1;
    }

    out_desc->req_ep_count = 0;
    out_desc->cap_count = 0;
    out_desc->entry_arg_binding_count = 0;

    /* The parser walks the blob linearly in the same order the packer writes it:
     * fixed header, name, entry, endpoint table, capability table, mem hints,
     * then raw WASM bytes. */
    uint32_t off = header_size;
    if (check_bounds(off, name_len, blob_size) != 0) {
        return -1;
    }
    const uint8_t* name = &blob[off];
    off += name_len;

    if (check_bounds(off, entry_len, blob_size) != 0) {
        return -1;
    }
    const uint8_t* entry = &blob[off];
    off += entry_len;

    for (uint32_t i = 0; i < req_ep_count; ++i) {
        if (check_bounds(off, sizeof(wasmos_req_endpoint_t), blob_size) != 0) {
            return -1;
        }
        const wasmos_req_endpoint_t* req = (const wasmos_req_endpoint_t*)&blob[off];
        off += sizeof(wasmos_req_endpoint_t);
        if (check_bounds(off, req->name_len, blob_size) != 0) {
            return -1;
        }
        out_desc->req_eps[i].name = &blob[off];
        out_desc->req_eps[i].name_len = req->name_len;
        out_desc->req_eps[i].rights = req->rights;
        off += req->name_len;
        out_desc->req_ep_count++;
    }

    for (uint32_t i = 0; i < cap_count; ++i) {
        if (check_bounds(off, sizeof(wasmos_cap_request_t), blob_size) != 0) {
            return -1;
        }
        const wasmos_cap_request_t* cap = (const wasmos_cap_request_t*)&blob[off];
        off += sizeof(wasmos_cap_request_t);
        if (check_bounds(off, cap->name_len, blob_size) != 0) {
            return -1;
        }
        out_desc->caps[i].name = &blob[off];
        out_desc->caps[i].name_len = cap->name_len;
        out_desc->caps[i].flags = cap->flags;
        off += cap->name_len;
        out_desc->cap_count++;
    }

    for (uint32_t i = 0; i < entry_arg_binding_count; ++i) {
        if (check_bounds(off, sizeof(wasmos_entry_arg_binding_t), blob_size) != 0) {
            return -1;
        }
        const wasmos_entry_arg_binding_t* binding = (const wasmos_entry_arg_binding_t*)&blob[off];
        off += sizeof(wasmos_entry_arg_binding_t);
        if (check_bounds(off, binding->name_len, blob_size) != 0) {
            return -1;
        }
        out_desc->entry_arg_bindings[i].name = &blob[off];
        out_desc->entry_arg_bindings[i].name_len = binding->name_len;
        off += binding->name_len;
        out_desc->entry_arg_binding_count++;
    }

    for (uint32_t i = 0; i < driver_match_count; ++i) {
        if (check_bounds(off, sizeof(wasmos_app_driver_match_t), blob_size) != 0) {
            return -1;
        }
        const wasmos_app_driver_match_t* m = (const wasmos_app_driver_match_t*)&blob[off];
        driver_matches[i] = *m;
        off += sizeof(wasmos_app_driver_match_t);
    }

    for (uint32_t i = 0; i < region_count; ++i) {
        if (check_bounds(off, sizeof(wasmos_app_region_t), blob_size) != 0) {
            return -1;
        }
        const wasmos_app_region_t* r = (const wasmos_app_region_t*)&blob[off];
        if (r->kind == WASMOS_APP_REGION_IO && r->first > r->last) {
            return -1; /* an inverted window would grant nothing and hide the typo */
        }
        if (r->kind == WASMOS_APP_REGION_BAR && r->bar_index >= 6u) {
            return -1;
        }
        out_desc->regions[i] = *r;
        out_desc->region_count++;
        off += sizeof(wasmos_app_region_t);
    }

    uint32_t stack_pages_hint = 0;
    uint32_t heap_pages_hint = 0;
    for (uint32_t i = 0; i < mem_hint_count; ++i) {
        if (check_bounds(off, sizeof(wasmos_mem_hint_t), blob_size) != 0) {
            return -1;
        }
        const wasmos_mem_hint_t* hint = (const wasmos_mem_hint_t*)&blob[off];
        off += sizeof(wasmos_mem_hint_t);
        if (hint->kind == WASMOS_APP_MEM_HINT_STACK) {
            stack_pages_hint = hint->min_pages;
        } else if (hint->kind == WASMOS_APP_MEM_HINT_HEAP) {
            heap_pages_hint = hint->min_pages;
        }
    }

    if (check_bounds(off, wasm_size, blob_size) != 0) {
        return -1;
    }

    const uint8_t* wasm_bytes = &blob[off];
    off += wasm_size;

    /* Version 4+ may append a pre-compiled WARP AOT binary after the WASM bytes. */
    const uint8_t* compiled_bytes = 0;
    if (compiled_size > 0) {
        if (check_bounds(off, compiled_size, blob_size) != 0) {
            return -1;
        }
        compiled_bytes = &blob[off];
        off += compiled_size;
    }

    out_desc->blob = blob;
    out_desc->blob_size = blob_size;
    out_desc->flags = flags;
    copy_subsystem_tag(out_desc->subsystem_tag, subsystem_tag);
    out_desc->name = name;
    out_desc->name_len = name_len;
    out_desc->entry = entry;
    out_desc->entry_len = entry_len;
    out_desc->wasm_bytes = wasm_bytes;
    out_desc->wasm_size = wasm_size;
    out_desc->compiled_bytes = compiled_bytes;
    out_desc->compiled_size = compiled_size;
    out_desc->stack_pages_hint = stack_pages_hint;
    out_desc->heap_pages_hint = heap_pages_hint;
    out_desc->driver_match_count = driver_match_count;
    for (uint32_t i = 0; i < driver_match_count; ++i) {
        out_desc->driver_matches[i] = driver_matches[i];
    }
    return 0;
}

int wasmos_app_resolve_subsystem(const wasmos_app_desc_t* desc,
                                 wasmos_app_subsystem_info_t* out_info) {
    if (!desc || !out_info) {
        return -1;
    }
    const wasmos_subsystem_registry_entry_t* handler =
        wasmos_app_find_subsystem_handler(desc->subsystem_tag);
    if (!handler) {
        return -1;
    }
    if (((desc->flags & WASMOS_APP_FLAG_NATIVE) != 0) != (handler->uses_wasm_payload == 0u)) {
        return -1;
    }
    copy_subsystem_tag(out_info->requested_tag, handler->request_tag);
    copy_subsystem_tag(out_info->runtime_tag, handler->runtime_tag);
    copy_subsystem_tag(out_info->broker_name, handler->broker_name);
    out_info->kind = handler->kind;
    out_info->uses_wasm_payload = handler->uses_wasm_payload;
    out_info->needs_runtime_lock = handler->needs_runtime_lock;
    out_info->gates_ready_for_services = handler->gates_ready_for_services;
    out_info->broker_endpoint = handler->broker_endpoint;
    out_info->ops = handler->ops;
    return 0;
}

int wasmos_app_requires_explicit_ready(const wasmos_app_desc_t* desc) {
    wasmos_app_subsystem_info_t info;
    if (!desc) {
        return -1;
    }
    if (wasmos_app_resolve_subsystem(desc, &info) != 0) {
        return -1;
    }
    if (info.kind != WASMOS_SUBSYSTEM_HANDLER_BUILTIN || !info.ops) {
        return -1;
    }
    if ((desc->flags & (WASMOS_APP_FLAG_SERVICE | WASMOS_APP_FLAG_DRIVER)) == 0) {
        return 0;
    }
    return info.gates_ready_for_services ? 1 : 0;
}

int wasmos_app_call_entry(wasmos_app_instance_t* instance) {
    if (!instance || !instance->active || !instance->ops || !instance->ops->call_entry) {
        trace_write("[wasmos-app] entry skipped (inactive)\n");
        return -1;
    }
    trace_do(
        klog_printf("[wasmos-app] entry start %s export=%s\n", instance->name, instance->entry));
    /* Entry dispatch is centralized here so drivers, services, and applications
     * all produce the same diagnostic framing around their actual export call. */
    int rc = instance->ops->call_entry(&instance->runtime, instance->entry, instance->entry_argc,
                                       instance->entry_argv);
    trace_do(klog_printf("[wasmos-app] entry rc=%016llx\n[wasmos-app] entry %s %s\n",
                         (unsigned long long)(uint32_t)rc, rc == 0 ? "ok" : "failed",
                         instance->name));
    return rc;
}

int wasmos_app_start(wasmos_app_instance_t* instance, const wasmos_app_desc_t* desc,
                     uint32_t owner_context_id, const uint32_t* init_argv, uint32_t init_argc) {
    if (!instance || !desc || owner_context_id == 0) {
        return -1;
    }
    if (str_copy_bytes(instance->name, sizeof(instance->name), desc->name, desc->name_len) != 0 ||
        str_copy_bytes(instance->entry, sizeof(instance->entry), desc->entry, desc->entry_len) !=
            0) {
        klog_write("[wasmos-app] invalid name or entry\n");
        return -1;
    }

    instance->resolved_ep_count = 0;
    for (uint32_t i = 0; i < desc->req_ep_count; ++i) {
        if (!g_endpoint_resolver) {
            klog_write("[wasmos-app] endpoint resolver missing\n");
            return -1;
        }
        uint32_t endpoint = IPC_ENDPOINT_NONE;
        if (g_endpoint_resolver(owner_context_id, desc->req_eps[i].name, desc->req_eps[i].name_len,
                                desc->req_eps[i].rights, &endpoint) != 0 ||
            endpoint == IPC_ENDPOINT_NONE) {
            klog_write("[wasmos-app] endpoint resolve failed\n");
            return -1;
        }
        instance->resolved_eps[instance->resolved_ep_count++] = endpoint;
    }

    for (uint32_t i = 0; i < desc->cap_count; ++i) {
        if (!g_capability_granter) {
            klog_write("[wasmos-app] capability granter missing\n");
            return -1;
        }
        if (g_capability_granter(owner_context_id, desc->caps[i].name, desc->caps[i].name_len,
                                 desc->caps[i].flags) != 0) {
            klog_write("[wasmos-app] capability grant failed\n");
            return -1;
        }
    }

    wasmos_app_subsystem_info_t subsystem_info;
    wasmos_app_start_params_t params;
    if (wasmos_app_resolve_subsystem(desc, &subsystem_info) != 0) {
        klog_write("[wasmos-app] subsystem resolve failed\n");
        return -1;
    }
    instance->ops = subsystem_info.ops;
    if (subsystem_info.kind != WASMOS_SUBSYSTEM_HANDLER_BUILTIN) {
        /* TODO: Route broker-backed subsystems through IPC once the first
         * userland subsystem broker lands. */
        klog_write("[wasmos-app] broker-backed subsystem start not implemented\n");
        return -1;
    }
    if (!instance->ops || !instance->ops->start) {
        klog_write("[wasmos-app] subsystem ops missing\n");
        return -1;
    }

    params.name = instance->name;
    params.module_bytes = desc->wasm_bytes;
    params.module_size = desc->wasm_size;
    params.compiled_bytes = desc->compiled_bytes;
    params.compiled_size = desc->compiled_size;
    params.entry_export = 0;
    params.entry_argc = 0;
    params.entry_argv = 0;
    params.stack_size = desc->stack_pages_hint ? desc->stack_pages_hint * 4096u : 64u * 1024u;
    params.heap_size = desc->heap_pages_hint ? desc->heap_pages_hint * 4096u : 64u * 1024u;

    if (init_argc > 4) {
        init_argc = 4;
    }
    instance->entry_argc = init_argc;
    for (uint32_t i = 0; i < 4; ++i) {
        instance->entry_argv[i] = init_argv ? init_argv[i] : 0;
    }
    params.entry_export = instance->entry;
    params.entry_argc = instance->entry_argc;
    params.entry_argv = instance->entry_argv;

    if (instance->ops->start(&instance->runtime, &params, owner_context_id, desc->flags) != 0) {
        klog_write("[wasmos-app] start failed\n");
        instance->ops = 0;
        return -1;
    }

    instance->flags = desc->flags;
    instance->owner_context_id = owner_context_id;
    instance->active = 1;
    return 0;
}

void wasmos_app_stop(wasmos_app_instance_t* instance) {
    if (!instance || !instance->active) {
        return;
    }
    if (instance->ops && instance->ops->stop) {
        instance->ops->stop(&instance->runtime);
    }
    instance->ops = 0;
    instance->active = 0;
    instance->flags = 0;
    instance->owner_context_id = 0;
    instance->resolved_ep_count = 0;
    instance->entry_argc = 0;
}

void wasmos_app_set_policy_hooks(wasmos_app_endpoint_resolver_t endpoint_resolver,
                                 wasmos_app_capability_granter_t capability_granter) {
    g_endpoint_resolver = endpoint_resolver;
    g_capability_granter = capability_granter;
}
