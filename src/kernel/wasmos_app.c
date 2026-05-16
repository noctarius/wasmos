#include <stddef.h>
#include "klog.h"
#include "wasmos_app.h"
#include "serial.h"

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

static int
check_u32_add(uint32_t a, uint32_t b, uint32_t *out)
{
    uint64_t sum = (uint64_t)a + (uint64_t)b;
    if (sum > 0xFFFFFFFFULL) {
        return -1;
    }
    *out = (uint32_t)sum;
    return 0;
}

static int
check_bounds(uint32_t offset, uint32_t size, uint32_t blob_size)
{
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

static int
copy_ascii_field(char *dst, uint32_t dst_size, const uint8_t *src, uint32_t src_len)
{
    if (!dst || !src || dst_size == 0 || src_len == 0 || src_len >= dst_size) {
        return -1;
    }
    for (uint32_t i = 0; i < src_len; ++i) {
        dst[i] = (char)src[i];
    }
    dst[src_len] = '\0';
    return 0;
}


int
wasmos_app_parse(const uint8_t *blob, uint32_t blob_size, wasmos_app_desc_t *out_desc)
{
    if (!blob || !out_desc || blob_size < sizeof(wasmos_app_header_v1_t)) {
        return -1;
    }
    const wasmos_app_header_v1_t *hdr_v1 = (const wasmos_app_header_v1_t *)blob;
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
    wasmos_app_driver_match_t driver_matches[WASMOS_APP_MAX_DRIVER_MATCHES];
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
        const wasmos_app_header_v2_t *hdr_v2 = (const wasmos_app_header_v2_t *)blob;
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
    } else if (version == WASMOS_APP_VERSION) {
        if (blob_size < sizeof(wasmos_app_header_v3_t)) {
            return -1;
        }
        const wasmos_app_header_v3_t *hdr_v3 = (const wasmos_app_header_v3_t *)blob;
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
    } else {
        return -1;
    }
    if ((version == 1u && header_size != sizeof(wasmos_app_header_v1_t)) ||
        (version == 2u && header_size != sizeof(wasmos_app_header_v2_t)) ||
        (version == WASMOS_APP_VERSION && header_size != sizeof(wasmos_app_header_v3_t)) ||
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
    /* NATIVE is only meaningful for drivers; reject any other combination. */
    if ((flags & WASMOS_APP_FLAG_NATIVE) &&
        !(flags & WASMOS_APP_FLAG_DRIVER)) {
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
    const uint8_t *name = &blob[off];
    off += name_len;

    if (check_bounds(off, entry_len, blob_size) != 0) {
        return -1;
    }
    const uint8_t *entry = &blob[off];
    off += entry_len;

    for (uint32_t i = 0; i < req_ep_count; ++i) {
        if (check_bounds(off, sizeof(wasmos_req_endpoint_t), blob_size) != 0) {
            return -1;
        }
        const wasmos_req_endpoint_t *req = (const wasmos_req_endpoint_t *)&blob[off];
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
        const wasmos_cap_request_t *cap = (const wasmos_cap_request_t *)&blob[off];
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
        const wasmos_entry_arg_binding_t *binding = (const wasmos_entry_arg_binding_t *)&blob[off];
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
        const wasmos_app_driver_match_t *m = (const wasmos_app_driver_match_t *)&blob[off];
        driver_matches[i] = *m;
        off += sizeof(wasmos_app_driver_match_t);
    }

    uint32_t stack_pages_hint = 0;
    uint32_t heap_pages_hint = 0;
    for (uint32_t i = 0; i < mem_hint_count; ++i) {
        if (check_bounds(off, sizeof(wasmos_mem_hint_t), blob_size) != 0) {
            return -1;
        }
        const wasmos_mem_hint_t *hint = (const wasmos_mem_hint_t *)&blob[off];
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

    out_desc->blob = blob;
    out_desc->blob_size = blob_size;
    out_desc->flags = flags;
    out_desc->name = name;
    out_desc->name_len = name_len;
    out_desc->entry = entry;
    out_desc->entry_len = entry_len;
    out_desc->wasm_bytes = &blob[off];
    out_desc->wasm_size = wasm_size;
    out_desc->stack_pages_hint = stack_pages_hint;
    out_desc->heap_pages_hint = heap_pages_hint;
    out_desc->driver_match_count = driver_match_count;
    for (uint32_t i = 0; i < driver_match_count; ++i) {
        out_desc->driver_matches[i] = driver_matches[i];
    }
    return 0;
}

int
wasmos_app_call_entry(wasmos_app_instance_t *instance)
{
    if (!instance || !instance->active) {
        klog_write("[wasmos-app] entry skipped (inactive)\n");
        return -1;
    }
    klog_printf("[wasmos-app] entry start %s export=%s\n",
        instance->name, instance->entry);
    /* Entry dispatch is centralized here so drivers, services, and applications
     * all produce the same diagnostic framing around their actual export call. */
    int rc = wasm_driver_call_unlocked(&instance->driver,
                                       instance->entry,
                                       instance->entry_argc,
                                       instance->entry_argv);
    klog_printf("[wasmos-app] entry rc=%016llx\n[wasmos-app] entry %s %s\n",
        (unsigned long long)(uint32_t)rc,
        rc == 0 ? "ok" : "failed",
        instance->name);
    return rc;
}

int
wasmos_app_start(wasmos_app_instance_t *instance,
                 const wasmos_app_desc_t *desc,
                 uint32_t owner_context_id,
                 const uint32_t *init_argv,
                 uint32_t init_argc)
{
    if (!instance || !desc || owner_context_id == 0) {
        return -1;
    }
    if (copy_ascii_field(instance->name, sizeof(instance->name), desc->name, desc->name_len) != 0 ||
        copy_ascii_field(instance->entry, sizeof(instance->entry), desc->entry, desc->entry_len) != 0) {
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
        if (g_endpoint_resolver(owner_context_id,
                                desc->req_eps[i].name,
                                desc->req_eps[i].name_len,
                                desc->req_eps[i].rights,
                                &endpoint) != 0 ||
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
        if (g_capability_granter(owner_context_id,
                                 desc->caps[i].name,
                                 desc->caps[i].name_len,
                                 desc->caps[i].flags) != 0) {
            klog_write("[wasmos-app] capability grant failed\n");
            return -1;
        }
    }

    wasm_driver_manifest_t manifest;
    manifest.name = instance->name;
    manifest.module_bytes = desc->wasm_bytes;
    manifest.module_size = desc->wasm_size;
    manifest.entry_export = 0;
    manifest.entry_argc = 0;
    manifest.entry_argv = 0;
    manifest.stack_size = desc->stack_pages_hint ? desc->stack_pages_hint * 4096u : 64u * 1024u;
    manifest.heap_size = desc->heap_pages_hint ? desc->heap_pages_hint * 4096u : 64u * 1024u;

    if (init_argc > 4) {
        init_argc = 4;
    }
    instance->entry_argc = init_argc;
    for (uint32_t i = 0; i < 4; ++i) {
        instance->entry_argv[i] = init_argv ? init_argv[i] : 0;
    }

    if (wasm_driver_start(&instance->driver, &manifest, owner_context_id) != 0) {
        klog_write("[wasmos-app] start failed\n");
        return -1;
    }

    instance->flags = desc->flags;
    instance->owner_context_id = owner_context_id;
    instance->active = 1;
    return 0;
}

void
wasmos_app_stop(wasmos_app_instance_t *instance)
{
    if (!instance || !instance->active) {
        return;
    }
    wasm_driver_stop(&instance->driver);
    instance->active = 0;
    instance->flags = 0;
    instance->owner_context_id = 0;
    instance->resolved_ep_count = 0;
    instance->entry_argc = 0;
}

void
wasmos_app_set_policy_hooks(wasmos_app_endpoint_resolver_t endpoint_resolver,
                            wasmos_app_capability_granter_t capability_granter)
{
    g_endpoint_resolver = endpoint_resolver;
    g_capability_granter = capability_granter;
}
