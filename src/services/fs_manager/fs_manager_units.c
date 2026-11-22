#include "fs_manager_units.h"

#include <string.h>

#include "wasmos_driver_abi.h"

int32_t fsmgr_unit_name(const char* module_path, const char* mount, char* out, uint32_t cap) {
    const char* stem;
    uint32_t stem_len = 0;
    uint32_t mount_len;
    uint32_t len;

    if (!out || cap == 0u) {
        return WASMOS_ERR_FS_PATH_TOO_LONG;
    }
    out[0] = '\0';
    if (!module_path || !mount) {
        return WASMOS_ERR_FS_PATH_TOO_LONG;
    }
    stem = strrchr(module_path, '/');
    stem = stem ? stem + 1 : module_path;
    while (stem[stem_len] != '\0' && stem[stem_len] != '.') {
        stem_len++;
    }
    mount_len = (uint32_t)strlen(mount);
    len = stem_len + 1u + mount_len;
    if (stem_len == 0u || len > SUPERVISOR_UNIT_NAME_MAX || len >= cap) {
        return WASMOS_ERR_FS_PATH_TOO_LONG;
    }
    memcpy(out, stem, stem_len);
    out[stem_len] = '@';
    memcpy(out + stem_len + 1u, mount, mount_len);
    out[len] = '\0';
    return (int32_t)len;
}

int32_t fsmgr_unit_spec(const char* name, const char* module_path, const char* args, char* out,
                        uint32_t cap) {
    static const char policy[] = " restart=on-failure -- ";
    uint32_t name_len;
    uint32_t path_len;
    uint32_t args_len;
    uint32_t len;
    uint32_t at = 0;

    if (!out || cap == 0u) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    out[0] = '\0';
    if (!name || !module_path || !args) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    name_len = (uint32_t)strlen(name);
    path_len = (uint32_t)strlen(module_path);
    args_len = (uint32_t)strlen(args);
    len = name_len + 1u + path_len + (uint32_t)(sizeof(policy) - 1u) + args_len;
    if (len > SUPERVISOR_SPEC_MAX || len >= cap) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    memcpy(out + at, name, name_len);
    at += name_len;
    out[at++] = ' ';
    memcpy(out + at, module_path, path_len);
    at += path_len;
    memcpy(out + at, policy, sizeof(policy) - 1u);
    at += (uint32_t)(sizeof(policy) - 1u);
    memcpy(out + at, args, args_len);
    at += args_len;
    out[at] = '\0';
    return (int32_t)at;
}

fsmgr_unit_t* fsmgr_unit_claim(fsmgr_unit_t* table, uint32_t cap, const char* name,
                               const char* mount) {
    fsmgr_unit_t* free_slot = 0;
    if (!table || !name || !mount || strlen(name) > SUPERVISOR_UNIT_NAME_MAX ||
        strlen(mount) >= FSMGR_MOUNT_PATH_MAX) {
        return 0;
    }
    for (uint32_t i = 0; i < cap; ++i) {
        if (table[i].state == FSMGR_UNIT_FREE) {
            if (!free_slot) {
                free_slot = &table[i];
            }
        } else if (strcmp(table[i].name, name) == 0) {
            return 0;
        }
    }
    if (!free_slot) {
        return 0;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->state = FSMGR_UNIT_SUBMITTING;
    strcpy(free_slot->name, name);
    strcpy(free_slot->mount, mount);
    free_slot->backend = -1;
    free_slot->client = -1;
    free_slot->client_request_id = 0;
    free_slot->request_id = 0;
    free_slot->buffer_id = -1;
    return free_slot;
}

fsmgr_unit_t* fsmgr_unit_by_mount(fsmgr_unit_t* table, uint32_t cap, const char* mount) {
    if (!table || !mount) {
        return 0;
    }
    for (uint32_t i = 0; i < cap; ++i) {
        if (table[i].state != FSMGR_UNIT_FREE && strcmp(table[i].mount, mount) == 0) {
            return &table[i];
        }
    }
    return 0;
}

fsmgr_unit_t* fsmgr_unit_by_request(fsmgr_unit_t* table, uint32_t cap, int32_t request_id) {
    if (!table) {
        return 0;
    }
    for (uint32_t i = 0; i < cap; ++i) {
        if ((table[i].state == FSMGR_UNIT_SUBMITTING || table[i].state == FSMGR_UNIT_RETRACTING) &&
            table[i].request_id == request_id) {
            return &table[i];
        }
    }
    return 0;
}

void fsmgr_unit_release(fsmgr_unit_t* unit) {
    if (unit) {
        memset(unit, 0, sizeof(*unit));
        unit->state = FSMGR_UNIT_FREE;
        unit->backend = -1;
        unit->client = -1;
        unit->buffer_id = -1;
    }
}

int32_t fsmgr_deadline_passed(uint32_t now_ms, uint32_t deadline_ms) {
    return (int32_t)(now_ms - deadline_ms) >= 0;
}
