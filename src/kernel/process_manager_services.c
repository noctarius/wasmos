#include "process_manager_internal.h"
#include "klog.h"
#include "process_manager.h"
#include "serial.h"
#include "string.h"

void
pm_unpack_name_args(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, char *out, uint32_t out_len)
{
    uint32_t args[4] = { arg0, arg1, arg2, arg3 };
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

void
pm_pack_name_args(const char *name, uint32_t out[4])
{
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

void
pm_update_well_known_service_endpoint(const char *name, uint32_t endpoint)
{
    typedef struct {
        const char *name;
        uint32_t *slot;
    } pm_service_slot_t;
    pm_service_slot_t slots[] = {
        {"block", &g_pm.block_endpoint},
        {"fs", &g_pm.fs_endpoint},
        {"fs.vfs", &g_pm.fs_endpoint},
        {"vt", &g_pm.vt_endpoint},
        {"fb", &g_pm.fb_endpoint},
    };
    if (!name) {
        return;
    }
    for (uint32_t i = 0; i < (uint32_t)(sizeof(slots) / sizeof(slots[0])); ++i) {
        if (strcmp(name, slots[i].name) == 0) {
            *slots[i].slot = endpoint;
            return;
        }
    }
}

int
pm_service_set(const char *name, uint32_t endpoint, uint32_t owner_context_id)
{
    pm_service_entry_t *empty = 0;
    for (uint32_t i = 0; i < PM_SERVICE_REGISTRY_CAP; ++i) {
        pm_service_entry_t *entry = &g_pm.services[i];
        if (!entry->in_use) {
            if (!empty) {
                empty = entry;
            }
            continue;
        }
        if (strcmp(entry->name, name) != 0) {
            continue;
        }
        if (entry->owner_context_id != owner_context_id) {
            return -1;
        }
        entry->endpoint = endpoint;
        return 0;
    }
    if (!empty) {
        return -1;
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

uint32_t
pm_service_lookup(const char *name)
{
    for (uint32_t i = 0; i < PM_SERVICE_REGISTRY_CAP; ++i) {
        if (!g_pm.services[i].in_use) {
            continue;
        }
        if (strcmp(g_pm.services[i].name, name) == 0) {
            return g_pm.services[i].endpoint;
        }
    }
    return IPC_ENDPOINT_NONE;
}

int
pm_handle_service_register(uint32_t pm_context_id, const ipc_message_t *msg)
{
    char name[17];
    uint32_t owner_context_id = 0;
    uint32_t endpoint_owner = 0;
    int track_fs = 0;
    ipc_message_t resp;
    pm_unpack_name_args((uint32_t)msg->arg0,
                        (uint32_t)msg->arg1,
                        (uint32_t)msg->arg2,
                        (uint32_t)msg->arg3,
                        name,
                        sizeof(name));
    if (name[0] == '\0') {
        return -1;
    }
    track_fs = (strcmp(name, "fs") == 0) || (strcmp(name, "fs.vfs") == 0);
    if (ipc_endpoint_owner(msg->source, &owner_context_id) != IPC_OK) {
        if (track_fs) klog_write("[pm] fs register owner lookup failed\n");
        return -1;
    }
    if (ipc_endpoint_owner(msg->source, &endpoint_owner) != IPC_OK ||
        endpoint_owner != owner_context_id) {
        if (track_fs) klog_write("[pm] fs register endpoint owner mismatch\n");
        return -1;
    }
    if (pm_service_set(name, msg->source, owner_context_id) != 0) {
        if (track_fs) klog_write("[pm] fs register service set failed\n");
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

int
pm_handle_service_lookup(uint32_t pm_context_id, const ipc_message_t *msg)
{
    char name[17];
    ipc_message_t resp;
    uint32_t endpoint = IPC_ENDPOINT_NONE;
    pm_unpack_name_args((uint32_t)msg->arg0,
                        (uint32_t)msg->arg1,
                        (uint32_t)msg->arg2,
                        (uint32_t)msg->arg3,
                        name,
                        sizeof(name));
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
