#ifndef WASMOS_PROCESS_MANAGER_INTERNAL_H
#define WASMOS_PROCESS_MANAGER_INTERNAL_H

#include <stdint.h>
#include "boot.h"
#include "ipc.h"
#include "process.h"
#include "wasmos_app.h"

#define PM_MAX_MANAGED_APPS 16u
#define PM_MAX_WAITERS 8u
#define PM_FS_BUFFER_SIZE (256u * 1024u)
#define PM_SERVICE_REGISTRY_CAP 32u

typedef struct {
    uint8_t in_use;
    uint32_t pid;
    uint32_t flags;
    const uint8_t *blob;
    uint32_t blob_size;
    uint8_t blob_storage[PM_FS_BUFFER_SIZE];
    uint8_t started;
    uint32_t entry_argc;
    uint32_t entry_arg0;
    uint32_t entry_arg1;
    uint32_t entry_arg2;
    uint32_t entry_arg3;
    wasmos_app_instance_t app;
    char name[64];
} pm_app_state_t;

typedef struct {
    uint8_t in_use;
    uint32_t pid;
    uint32_t reply_endpoint;
    uint32_t request_id;
    uint32_t owner_context_id;
} pm_wait_state_t;

typedef struct {
    uint8_t in_use;
    uint32_t reply_endpoint;
    uint32_t request_id;
    uint32_t parent_pid;
    uint32_t fs_request_id;
    char name[32];
} pm_spawn_state_t;

typedef struct {
    uint8_t valid;
    uint32_t cap_flags;
    uint16_t io_port_min;
    uint16_t io_port_max;
    uint16_t irq_mask;
} pm_spawn_caps_t;

typedef struct {
    uint8_t in_use;
    uint32_t endpoint;
    uint32_t owner_context_id;
    char name[17];
} pm_service_entry_t;

typedef struct {
    const boot_info_t *boot_info;
    uint32_t proc_endpoint;
    uint32_t fs_endpoint;
    uint32_t block_endpoint;
    uint32_t fb_endpoint;
    uint32_t vt_endpoint;
    uint32_t fs_reply_endpoint;
    uint32_t fs_request_id;
    uint32_t next_cli_tty;
    uint8_t started;
    uint32_t init_module_index;
    uint32_t module_count;
    pm_app_state_t apps[PM_MAX_MANAGED_APPS];
    pm_wait_state_t waits[PM_MAX_WAITERS];
    pm_spawn_state_t spawn;
    pm_service_entry_t services[PM_SERVICE_REGISTRY_CAP];
} pm_state_t;

extern pm_state_t g_pm;
extern uint8_t g_pm_wait_owner_deny_logged;
extern uint8_t g_pm_kill_owner_deny_logged;
extern uint8_t g_pm_status_owner_deny_logged;
extern uint8_t g_pm_spawn_owner_deny_logged;

void pm_unpack_name_args(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, char *out, uint32_t out_len);
void pm_pack_name_args(const char *name, uint32_t out[4]);
uint32_t pm_alloc_cli_tty(void);

int pm_service_set(const char *name, uint32_t endpoint, uint32_t owner_context_id);
uint32_t pm_service_lookup(const char *name);
void pm_update_well_known_service_endpoint(const char *name, uint32_t endpoint);
int pm_handle_service_register(uint32_t pm_context_id, const ipc_message_t *msg);
int pm_handle_service_lookup(uint32_t pm_context_id, const ipc_message_t *msg);

int pm_handle_spawn(uint32_t pm_context_id, const ipc_message_t *msg);
int pm_handle_spawn_caps(uint32_t pm_context_id, const ipc_message_t *msg);
int pm_handle_spawn_name(uint32_t pm_context_id, const ipc_message_t *msg);
uint32_t pm_find_module_index_by_name(const char *name);
void pm_poll_spawn(uint32_t pm_context_id);
void pm_check_waits(uint32_t pm_context_id);
void pm_reap_apps(process_t *owner);

int pm_handle_module_meta(uint32_t pm_context_id, const ipc_message_t *msg);
int pm_handle_module_meta_path(uint32_t pm_context_id, const ipc_message_t *msg);

#endif
