/* process_manager_services.c - PM service registration and discovery.
 * Maintains a name → endpoint lookup table for registered services.
 * SVC_IPC_REGISTER_REQ / SVC_IPC_LOOKUP_REQ messages are handled here. */
#include "process_manager_internal.h"
#include "capability.h"
#include "klog.h"
#include "process.h"
#include "process_manager.h"
#include "service_class_registry.h"
#include "string.h"
#include "subsystem_registry.h"

/* PM context that owns the endpoints class-event pushes are sent from, plus a
 * one-time event-sink install. Set lazily by any class handler / the reap tick,
 * all of which carry the PM context id. */
static uint32_t g_svc_class_ctx;
static uint8_t g_svc_class_sink_set;

/* Push an SVC_IPC_CLASS_EVENT to a subscriber's notify endpoint. */
static void pm_service_class_event_sink(void* user, uint32_t notify_endpoint, uint32_t event,
                                        const char* class_name, uint32_t instance,
                                        uint32_t endpoint, uint32_t pid) {
    ipc_message_t ev;
    (void)user;
    (void)class_name;
    ev.type = SVC_IPC_CLASS_EVENT;
    ev.source = g_pm.proc_endpoint;
    ev.destination = notify_endpoint;
    ev.request_id = 0;
    ev.arg0 = event;
    ev.arg1 = instance;
    ev.arg2 = endpoint;
    ev.arg3 = pid;
    (void)ipc_send_from(g_svc_class_ctx, notify_endpoint, &ev);
}

/* A provider/subscriber owner is alive while its process exists and has not
 * exited (process_get_exit_status returns 0 only once a status is available). */
static int pm_service_class_alive(void* user, uint32_t owner_ctx) {
    process_t* p = process_find_by_context(owner_ctx);
    int32_t status = 0;
    (void)user;
    if (!p) {
        return 0;
    }
    return process_get_exit_status(p->pid, &status) != 0;
}

static void pm_service_class_ensure(uint32_t pm_context_id) {
    g_svc_class_ctx = pm_context_id;
    if (!g_svc_class_sink_set) {
        service_class_registry_set_event_sink(pm_service_class_event_sink, 0);
        g_svc_class_sink_set = 1;
    }
}

/* Periodic exit-driven sweep: purge class providers/subscribers whose owner has
 * died, firing REMOVE events. Called from the PM run loop's reap path. */
void pm_services_class_reap(uint32_t pm_context_id) {
    pm_service_class_ensure(pm_context_id);
    service_class_registry_reap_dead(pm_service_class_alive, 0);
}

void pm_unpack_name_args(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, char* out,
                         uint32_t out_len) {
    uint32_t args[4] = {arg0, arg1, arg2, arg3};
    uint32_t pos = 0;
    if (!out || out_len == 0) {
        return;
    }
    for (uint32_t i = 0; i < 4 && pos + 1 < out_len; ++i) {
        uint32_t v = args[i];
        for (uint32_t b = 0; b < 4 && pos + 1 < out_len; ++b) {
            char c = (char)(v & 0xFF);
            if (c == '\0') {
                out[pos] = '\0';
                return;
            }
            out[pos++] = c;
            v >>= 8;
        }
    }
    out[pos] = '\0';
}

void pm_pack_name_args(const char* name, uint32_t out[4]) {
    if (!out) {
        return;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        out[i] = 0;
    }
    if (!name) {
        return;
    }
    uint32_t idx = 0;
    for (uint32_t i = 0; name[i] && idx < 16; ++i, ++idx) {
        uint32_t slot = idx / 4;
        uint32_t shift = (idx % 4) * 8;
        out[slot] |= ((uint32_t)(uint8_t)name[i]) << shift;
    }
}

void pm_update_well_known_service_endpoint(const char* name, uint32_t endpoint) {
    if (!name) {
        return;
    }
    if (strcmp(name, "fs.vfs") == 0) {
        pm_atomic_store_u32(&g_pm.fs_endpoint, endpoint);
        return;
    }
    if (strcmp(name, "fs") == 0) {
        /* Keep path-based process spawns on VFS once it is available. */
        if (pm_atomic_load_u32(&g_pm.fs_endpoint) == IPC_ENDPOINT_NONE) {
            pm_atomic_store_u32(&g_pm.fs_endpoint, endpoint);
        }
        return;
    }
    if (strcmp(name, "block") == 0) {
        pm_atomic_store_u32(&g_pm.block_endpoint, endpoint);
        return;
    }
    if (strcmp(name, "vt") == 0) {
        pm_atomic_store_u32(&g_pm.vt_endpoint, endpoint);
        return;
    }
    if (strcmp(name, "fb") == 0) {
        pm_atomic_store_u32(&g_pm.fb_endpoint, endpoint);
        return;
    }
}

int pm_service_set(const char* name, uint32_t endpoint, uint32_t owner_context_id) {
    pm_service_entry_t* empty = 0;
    list_iter_t it;
    pm_service_entry_t* entry = (pm_service_entry_t*)list_first(&g_pm.services, &it);
    while (entry) {
        if (!entry->in_use) {
            if (!empty) {
                empty = entry;
            }
            entry = (pm_service_entry_t*)list_next(&it);
            continue;
        }
        if (strcmp(entry->name, name) != 0) {
            entry = (pm_service_entry_t*)list_next(&it);
            continue;
        }
        if (entry->owner_context_id != owner_context_id) {
            return -1;
        }
        entry->endpoint = endpoint;
        return 0;
    }
    if (!empty) {
        empty = (pm_service_entry_t*)list_alloc(&g_pm.services);
        if (!empty) {
            return -1;
        }
    }
    empty->in_use = 1;
    empty->endpoint = endpoint;
    empty->owner_context_id = owner_context_id;
    for (uint32_t i = 0; i < sizeof(empty->name); ++i) {
        empty->name[i] = name[i];
        if (!name[i]) {
            break;
        }
    }
    return 0;
}

uint32_t pm_service_lookup(const char* name) {
    list_iter_t it;
    pm_service_entry_t* entry = (pm_service_entry_t*)list_first(&g_pm.services, &it);
    while (entry) {
        if (entry->in_use && strcmp(entry->name, name) == 0) {
            return entry->endpoint;
        }
        entry = (pm_service_entry_t*)list_next(&it);
    }
    return IPC_ENDPOINT_NONE;
}

int pm_handle_service_register(uint32_t pm_context_id, const ipc_message_t* msg) {
    char name[17];
    uint32_t owner_context_id = 0;
    uint32_t endpoint_owner = 0;
    int track_fs = 0;
    ipc_message_t resp;
    pm_unpack_name_args((uint32_t)msg->arg0, (uint32_t)msg->arg1, (uint32_t)msg->arg2,
                        (uint32_t)msg->arg3, name, sizeof(name));
    if (name[0] == '\0') {
        return -1;
    }
    track_fs = (strcmp(name, "fs") == 0) || (strcmp(name, "fs.vfs") == 0);
    if (ipc_endpoint_owner(msg->source, &owner_context_id) != IPC_OK) {
        if (track_fs)
            klog_write("[pm] fs register owner lookup failed\n");
        return -1;
    }
    if (ipc_endpoint_owner(msg->source, &endpoint_owner) != IPC_OK ||
        endpoint_owner != owner_context_id) {
        if (track_fs)
            klog_write("[pm] fs register endpoint owner mismatch\n");
        return -1;
    }
    if (pm_service_set(name, msg->source, owner_context_id) != 0) {
        if (track_fs)
            klog_write("[pm] fs register service set failed\n");
        return -1;
    }
    pm_update_well_known_service_endpoint(name, msg->source);
    resp.type = SVC_IPC_REGISTER_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = 0;
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
}

/* Descriptor-based service registration (SVC_IPC_REGISTER_DESC_REQ).
 *
 * Unlike pm_handle_service_register(), the request carries a svc_register_desc_t
 * in the caller's xfer buffer (arg0=offset 0, arg1=byte length) and msg->source
 * is a DEDICATED reply endpoint, separate from the service endpoint being
 * registered (desc->service_endpoint).  This keeps the reply off the live
 * service endpoint, eliminating the reply/serve-traffic race that deadlocked
 * boot once the process manager stopped busy-polling. */
int pm_handle_service_register_desc(uint32_t pm_context_id, const ipc_message_t* msg) {
    char name[WASMOS_SVC_NAME_MAX];
    uint32_t reply_owner = 0;
    uint32_t service_owner = 0;
    uint32_t service_ep = IPC_ENDPOINT_NONE;
    uint32_t len = (uint32_t)msg->arg1;
    int track_fs = 0;
    ipc_message_t resp;
    const svc_register_desc_t* desc;
    uint32_t i;

    if (ipc_endpoint_owner(msg->source, &reply_owner) != IPC_OK) {
        return -1;
    }
    if (len < WASMOS_SVC_REGISTER_DESC_V1_BYTES || len > xfer_buffer_size(BUFFER_KIND_TRANSFER)) {
        return -1;
    }
    /* The descriptor lives in the caller's own transfer buffer; arg2 carries its
     * buffer_id (the caller owns it). PM reads it directly (kernel). */
    desc = (const svc_register_desc_t*)pm_foreign_xfer_ptr((uint32_t)msg->arg2, reply_owner, 0);
    if (!desc || desc->version < 1u || desc->version > WASMOS_SVC_REGISTER_DESC_VERSION) {
        return -1;
    }
    /* Copy the name out of the shared buffer and force NUL termination. */
    for (i = 0; i + 1u < sizeof(name) && desc->name[i] != '\0'; ++i) {
        name[i] = desc->name[i];
    }
    name[i] = '\0';
    if (name[0] == '\0') {
        return -1;
    }
    service_ep = desc->service_endpoint;
    track_fs = (strcmp(name, "fs") == 0) || (strcmp(name, "fs.vfs") == 0);
    /* The endpoint being registered must belong to the same context as the
     * reply endpoint — a caller may only register its own endpoints. */
    if (ipc_endpoint_owner(service_ep, &service_owner) != IPC_OK || service_owner != reply_owner) {
        if (track_fs)
            klog_write("[pm] fs register endpoint owner mismatch\n");
        return -1;
    }
    if (pm_service_set(name, service_ep, reply_owner) != 0) {
        if (track_fs)
            klog_write("[pm] fs register service set failed\n");
        return -1;
    }
    pm_update_well_known_service_endpoint(name, service_ep);
    /* v2+ descriptors may additionally register the service under a virtual
     * class; only holders of CAP_SVC_CLASS_REGISTER may claim a class. */
    if (desc->version >= 2u && len >= sizeof(svc_register_desc_t) && desc->class_name[0] != '\0') {
        char class_name[WASMOS_SVC_CLASS_MAX];
        process_t* provider;
        uint32_t provider_pid;
        if (!capability_has(reply_owner, CAP_SVC_CLASS_REGISTER)) {
            return -1;
        }
        str_copy(class_name, sizeof(class_name), desc->class_name);
        pm_service_class_ensure(pm_context_id);
        provider = process_find_by_context(reply_owner);
        provider_pid = provider ? provider->pid : 0;
        if (service_class_registry_add(class_name, desc->instance, service_ep, reply_owner,
                                       provider_pid) != 0) {
            return -1;
        }
    }
    resp.type = SVC_IPC_REGISTER_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = 0;
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
}

int pm_handle_service_lookup(uint32_t pm_context_id, const ipc_message_t* msg) {
    char name[17];
    ipc_message_t resp;
    uint32_t endpoint = IPC_ENDPOINT_NONE;
    pm_unpack_name_args((uint32_t)msg->arg0, (uint32_t)msg->arg1, (uint32_t)msg->arg2,
                        (uint32_t)msg->arg3, name, sizeof(name));
    if (name[0] == '\0') {
        return -1;
    }
    endpoint = pm_service_lookup(name);
    resp.type = SVC_IPC_LOOKUP_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = (endpoint == IPC_ENDPOINT_NONE) ? (uint32_t)-1 : endpoint;
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
}

/* Enumerate providers of a virtual class. The caller's transfer buffer (arg0)
 * carries the class name NUL-terminated at offset 0 on input; PM overwrites it
 * with a svc_class_entry_t[] and replies with the total match count (arg1 caps
 * how many entries the buffer holds). */
int pm_handle_service_lookup_class(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t owner = 0;
    uint32_t buffer_id = (uint32_t)msg->arg0;
    uint32_t max_entries = (uint32_t)msg->arg1;
    char class_name[WASMOS_SVC_CLASS_MAX];
    uint32_t cap_entries;
    uint32_t count;
    uint8_t* buf;
    ipc_message_t resp;

    if (ipc_endpoint_owner(msg->source, &owner) != IPC_OK) {
        return -1;
    }
    /* Cast away const: the caller owns this transfer buffer and hands it in for
     * PM to fill with the result array (same write pattern as FS-read replies). */
    buf = ptr_cast(uint8_t, pm_foreign_xfer_ptr(buffer_id, owner, 0));
    if (!buf) {
        return -1;
    }
    str_copy(class_name, sizeof(class_name), (const char*)buf);
    cap_entries = xfer_buffer_size(BUFFER_KIND_TRANSFER) / (uint32_t)sizeof(svc_class_entry_t);
    if (max_entries > cap_entries) {
        max_entries = cap_entries;
    }
    count = service_class_registry_lookup(class_name, (service_class_provider_t*)buf, max_entries);
    resp.type = SVC_IPC_LOOKUP_CLASS_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = count;
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
}

/* Subscribe to existence events for a class. arg0 is the caller's notify
 * endpoint (must belong to the caller); arg1 is a transfer buffer holding the
 * class name NUL-terminated at offset 0. */
int pm_handle_class_subscribe(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t owner = 0;
    uint32_t notify_endpoint = (uint32_t)msg->arg0;
    uint32_t buffer_id = (uint32_t)msg->arg1;
    uint32_t ne_owner = 0;
    char class_name[WASMOS_SVC_CLASS_MAX];
    const uint8_t* buf;
    ipc_message_t resp;

    if (ipc_endpoint_owner(msg->source, &owner) != IPC_OK) {
        return -1;
    }
    if (ipc_endpoint_owner(notify_endpoint, &ne_owner) != IPC_OK || ne_owner != owner) {
        return -1;
    }
    buf = pm_foreign_xfer_ptr(buffer_id, owner, 0);
    if (!buf) {
        return -1;
    }
    str_copy(class_name, sizeof(class_name), (const char*)buf);
    pm_service_class_ensure(pm_context_id);
    if (service_class_registry_subscribe(class_name, notify_endpoint, owner) != 0) {
        return -1;
    }
    resp.type = SVC_IPC_SUBSCRIBE_CLASS_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = 0;
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
}

int pm_handle_subsystem_register_broker(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t owner_context = 0;
    uint32_t endpoint_owner = 0;
    uint32_t len = (uint32_t)msg->arg1;
    const wasmos_subsystem_broker_register_desc_t* desc = 0;
    ipc_message_t resp;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return -(int32_t)WASMOS_ERR_PROC_PM_BAD_ENDPOINT;
    }
    if (!capability_has(owner_context, CAP_SUBSYSTEM_REGISTER)) {
        return -(int32_t)WASMOS_ERR_PROC_PM_NOT_AUTHORIZED;
    }
    if (len != sizeof(*desc) || len > xfer_buffer_size(BUFFER_KIND_TRANSFER)) {
        return -(int32_t)WASMOS_ERR_PROC_PM_BAD_BROKER;
    }
    desc = (const wasmos_subsystem_broker_register_desc_t*)pm_foreign_xfer_ptr((uint32_t)msg->arg2,
                                                                               owner_context, 0);
    if (!desc || desc->version != WASMOS_SUBSYSTEM_REGISTER_BROKER_DESC_VERSION ||
        desc->broker_endpoint == IPC_ENDPOINT_NONE) {
        return -(int32_t)WASMOS_ERR_PROC_PM_BAD_BROKER;
    }
    if (ipc_endpoint_owner(desc->broker_endpoint, &endpoint_owner) != IPC_OK ||
        endpoint_owner != owner_context) {
        return -(int32_t)WASMOS_ERR_PROC_PM_BAD_BROKER;
    }
    if (wasmos_subsystem_registry_register_broker(
            desc->request_tag, desc->runtime_tag, desc->broker_name, desc->broker_endpoint,
            owner_context, desc->uses_wasm_payload, desc->needs_runtime_lock,
            desc->gates_ready_for_services) != 0) {
        return -(int32_t)WASMOS_ERR_PROC_PM_SUBSYSTEM_REG;
    }
    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = 0;
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -(int32_t)WASMOS_ERR_PROC_PM_REPLY_SEND;
}

int pm_handle_exec_handler_register(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t owner_context = 0;
    uint32_t broker_owner = 0;
    uint32_t len = (uint32_t)msg->arg1;
    uint32_t node_bytes = 0;
    const wasmos_exec_handler_register_desc_t* desc = 0;
    const wasmos_subsystem_registry_entry_t* owner = 0;
    const wasmos_exec_match_node_t* nodes = 0;
    ipc_message_t resp;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return -(int32_t)WASMOS_ERR_PROC_PM_BAD_ENDPOINT;
    }
    if (!capability_has(owner_context, CAP_SUBSYSTEM_REGISTER)) {
        return -(int32_t)WASMOS_ERR_PROC_PM_NOT_AUTHORIZED;
    }
    if (len < sizeof(*desc) || len > xfer_buffer_size(BUFFER_KIND_TRANSFER)) {
        return -(int32_t)WASMOS_ERR_PROC_PM_BAD_HANDLER;
    }
    desc = (const wasmos_exec_handler_register_desc_t*)pm_foreign_xfer_ptr((uint32_t)msg->arg2,
                                                                           owner_context, 0);
    if (!desc || desc->version != WASMOS_EXEC_HANDLER_REGISTER_DESC_VERSION ||
        desc->node_count == 0u || desc->node_count > WASMOS_EXEC_MATCH_MAX_NODES) {
        return -(int32_t)WASMOS_ERR_PROC_PM_BAD_HANDLER;
    }
    node_bytes = desc->node_count * (uint32_t)sizeof(wasmos_exec_match_node_t);
    if (len != sizeof(*desc) + node_bytes) {
        return -(int32_t)WASMOS_ERR_PROC_PM_BAD_HANDLER;
    }
    owner = wasmos_subsystem_registry_find(desc->request_tag);
    if (!owner || owner->kind != WASMOS_SUBSYSTEM_HANDLER_BROKER) {
        return -(int32_t)WASMOS_ERR_PROC_PM_BAD_HANDLER;
    }
    if (ipc_endpoint_owner(owner->broker_endpoint, &broker_owner) != IPC_OK ||
        broker_owner != owner_context) {
        return -(int32_t)WASMOS_ERR_PROC_PM_BAD_HANDLER;
    }
    nodes = (const wasmos_exec_match_node_t*)((const uint8_t*)desc + sizeof(*desc));
    if (wasmos_subsystem_registry_register_exec_handler(
            desc->handler_name, desc->request_tag, owner_context, desc->priority,
            desc->max_probe_bytes, nodes, desc->node_count, desc->root_index) != 0) {
        return -(int32_t)WASMOS_ERR_PROC_PM_HANDLER_REG;
    }
    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = 0;
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -(int32_t)WASMOS_ERR_PROC_PM_REPLY_SEND;
}
