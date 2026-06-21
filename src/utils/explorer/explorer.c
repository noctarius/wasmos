#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unistd.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libui.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

#define EXPLORER_W 680
#define EXPLORER_H 420
#define EXPLORER_LIST_BUF 4096
#define EXPLORER_MAX_ENTRIES 96
#define EXPLORER_NAME_MAX 96
#define EXPLORER_PATH_MAX 256
#define EXPLORER_ROOT_PADDING 10
#define EXPLORER_ROOT_GAP 8
#define EXPLORER_PATH_H 30
#define EXPLORER_STATUS_H 26
#define EXPLORER_TOOLBAR_H 34
#define EXPLORER_BODY_H 286
#define EXPLORER_SIDEBAR_W 220
#define EXPLORER_TREE_MAX 128

typedef struct {
    char name[EXPLORER_NAME_MAX];
    int32_t is_dir;
} explorer_entry_t;

typedef struct {
    char path[EXPLORER_PATH_MAX];
    char label[EXPLORER_NAME_MAX + 12];
    int32_t depth;
} explorer_tree_node_t;

static int32_t
explorer_list_height(void)
{
    const int32_t fixed_h =
        (EXPLORER_ROOT_PADDING * 2) +
        EXPLORER_PATH_H +
        EXPLORER_STATUS_H +
        EXPLORER_TOOLBAR_H +
        EXPLORER_BODY_H +
        (EXPLORER_ROOT_GAP * 4);
    const int32_t remaining = EXPLORER_H - fixed_h;
    return remaining > 80 ? remaining : 80;
}

static ui_context_t g_ctx;
static int32_t g_path_label_id = -1;
static int32_t g_status_label_id = -1;
static int32_t g_open_button_id = -1;
static int32_t g_up_button_id = -1;
static int32_t g_refresh_button_id = -1;
static int32_t g_toolbar_row_id = -1;
static int32_t g_body_row_id = -1;
static int32_t g_tree_id = -1;
static int32_t g_content_panel_id = -1;
static int32_t g_list_id = -1;
static int32_t g_fs_reply_endpoint = -1;
static int32_t g_proc_endpoint = -1;
static int32_t g_proc_reply_endpoint = -1;

static char g_current_path[EXPLORER_PATH_MAX] = "/";
static char g_listbuf[EXPLORER_LIST_BUF];
static char g_status_buf[128];
static explorer_entry_t g_entries[EXPLORER_MAX_ENTRIES];
static explorer_tree_node_t g_tree_nodes[EXPLORER_TREE_MAX];
static int32_t g_entry_count = 0;
static int32_t g_tree_count = 0;

static int
explorer_fs_request(int32_t type,
                    int32_t arg0,
                    int32_t arg1,
                    int32_t arg2,
                    int32_t arg3,
                    int32_t *out_arg0)
{
    wasmos_ipc_message_t reply;
    const int32_t fs_endpoint = wasmos_fs_endpoint();

    if (fs_endpoint < 0 || g_fs_reply_endpoint < 0) {
        return -1;
    }
    if (wasmos_ipc_call(fs_endpoint,
                        g_fs_reply_endpoint,
                        type,
                        g_ctx.req_id++,
                        arg0,
                        arg1,
                        arg2,
                        arg3,
                        &reply) != 0) {
        return -1;
    }
    if (reply.type != FS_IPC_RESP) {
        return -1;
    }
    if (out_arg0) {
        *out_arg0 = reply.arg0;
    }
    return 0;
}

static ssize_t
explorer_fs_request_stream(int32_t type,
                           int32_t arg0,
                           int32_t arg1,
                           int32_t arg2,
                           int32_t arg3,
                           char *out,
                           size_t out_cap)
{
    wasmos_ipc_message_t reply;
    const int32_t fs_endpoint = wasmos_fs_endpoint();
    size_t out_len = 0;

    if (fs_endpoint < 0 || g_fs_reply_endpoint < 0 || !out || out_cap == 0) {
        return -1;
    }
    if (wasmos_ipc_send(fs_endpoint,
                        g_fs_reply_endpoint,
                        type,
                        g_ctx.req_id++,
                        arg0,
                        arg1,
                        arg2,
                        arg3) != 0) {
        return -1;
    }
    for (;;) {
        if (wasmos_ipc_select_one(g_fs_reply_endpoint) < 0) {
            return -1;
        }
        wasmos_ipc_message_read_last(&reply);
        if (reply.type == FS_IPC_STREAM) {
            int32_t args[4] = { reply.arg0, reply.arg1, reply.arg2, reply.arg3 };
            for (int32_t i = 0; i < 4; ++i) {
                char c = (char)(args[i] & 0xFF);
                if (c == '\0') {
                    continue;
                }
                if (out_len + 1 >= out_cap) {
                    out[out_cap - 1] = '\0';
                    return (ssize_t)out_len;
                }
                out[out_len++] = c;
            }
            continue;
        }
        if (reply.type != FS_IPC_RESP || reply.arg0 != 0) {
            return -1;
        }
        out[out_len < out_cap ? out_len : (out_cap - 1)] = '\0';
        return (ssize_t)out_len;
    }
}

static void
explorer_set_status(const char *text)
{
    ui_component_set_text(&g_ctx, g_status_label_id, text ? text : "");
}

static int
explorer_str_ends_with(const char *s, const char *suffix)
{
    size_t s_len;
    size_t suffix_len;

    if (!s || !suffix) {
        return 0;
    }
    s_len = strlen(s);
    suffix_len = strlen(suffix);
    if (suffix_len > s_len) {
        return 0;
    }
    return memcmp(s + (s_len - suffix_len), suffix, suffix_len) == 0;
}

static void
explorer_build_selected_path(char *out, size_t out_cap, const explorer_entry_t *entry)
{
    if (!out || out_cap == 0 || !entry) {
        return;
    }
    if (strcmp(g_current_path, "/") == 0) {
        snprintf(out, out_cap, "/%s", entry->name);
    } else {
        snprintf(out, out_cap, "%s/%s", g_current_path, entry->name);
    }
}

static void
explorer_update_path_label(void)
{
    char line[EXPLORER_PATH_MAX + 16];

    snprintf(line, sizeof(line), "Path: %s", g_current_path);
    ui_component_set_text(&g_ctx, g_path_label_id, line);
}

static int explorer_chdir_root(void);
static int explorer_chdir_name(const char *name);

static int
explorer_path_is_prefix(const char *prefix, const char *path)
{
    size_t prefix_len;

    if (!prefix || !path) return 0;
    if (strcmp(prefix, "/") == 0) return path[0] == '/';
    prefix_len = strlen(prefix);
    if (strncmp(prefix, path, prefix_len) != 0) return 0;
    return path[prefix_len] == '\0' || path[prefix_len] == '/';
}

static int
explorer_chdir_abs(const char *path)
{
    const char *seg = NULL;
    const char *cur = NULL;
    char name[EXPLORER_NAME_MAX];
    size_t len = 0;

    if (!path || path[0] != '/') return -1;
    if (explorer_chdir_root() != 0) return -1;
    if (strcmp(path, "/") == 0) return 0;

    cur = path + 1;
    while (*cur) {
        seg = cur;
        while (*cur && *cur != '/') cur++;
        len = (size_t)(cur - seg);
        if (len == 0) {
            if (*cur == '/') {
                cur++;
                continue;
            }
            break;
        }
        if (len >= sizeof(name)) return -1;
        memcpy(name, seg, len);
        name[len] = '\0';
        if (explorer_chdir_name(name) != 0) return -1;
        if (*cur == '/') cur++;
    }
    return 0;
}

static int
explorer_collect_entries_for_path(const char *path,
                                  explorer_entry_t *entries,
                                  int32_t max_entries,
                                  int32_t dirs_only,
                                  int32_t *out_count)
{
    char saved_path[EXPLORER_PATH_MAX];
    int32_t count = 0;
    ssize_t nread;
    int32_t start = 0;

    if (!path || !entries || max_entries <= 0) return -1;
    strncpy(saved_path, g_current_path, sizeof(saved_path) - 1);
    saved_path[sizeof(saved_path) - 1] = '\0';

    if (explorer_chdir_abs(path) != 0) {
        (void)explorer_chdir_abs(saved_path);
        return -1;
    }
    nread = explorer_fs_request_stream(FS_IPC_READDIR_REQ, 0, 0, 0, 0, g_listbuf, sizeof(g_listbuf));
    (void)explorer_chdir_abs(saved_path);
    if (nread < 0) return -1;

    for (int32_t i = 0; i <= (int32_t)nread && count < max_entries; ++i) {
        if (g_listbuf[i] != '\n' && g_listbuf[i] != '\0') continue;
        if (i > start) {
            int32_t raw_len = i - start;
            int32_t is_dir = 0;
            int32_t copy_len = raw_len;
            explorer_entry_t *entry = &entries[count];

            if (g_listbuf[i - 1] == '/') {
                is_dir = 1;
                copy_len--;
            }
            if (dirs_only && !is_dir) {
                start = i + 1;
                continue;
            }
            if (copy_len >= EXPLORER_NAME_MAX) copy_len = EXPLORER_NAME_MAX - 1;
            if (copy_len > 0) memcpy(entry->name, g_listbuf + start, (size_t)copy_len);
            entry->name[copy_len] = '\0';
            entry->is_dir = is_dir;
            if (entry->name[0] != '\0' &&
                strcmp(entry->name, ".") != 0 &&
                strcmp(entry->name, "..") != 0) {
                count++;
            }
        }
        start = i + 1;
    }
    if (out_count) *out_count = count;
    return 0;
}

static void
explorer_list_view_clear(int32_t list_id)
{
    ui_component_t *list = ui_component_by_id(&g_ctx, list_id);
    ui_list_view_data_t *data;

    if (!list || list->type != UI_COMPONENT_LIST_VIEW || !list->component_data) {
        return;
    }
    data = (ui_list_view_data_t *)list->component_data;
    for (int32_t i = 0; i < data->list.count; ++i) {
        if (data->list.items && data->list.items[i]) {
            free(data->list.items[i]);
            data->list.items[i] = NULL;
        }
    }
    data->list.count = 0;
    data->list.selected = -1;
    data->scroll_y = 0;
    data->scroll_max = 0;
}

static int32_t
explorer_selected_index(void)
{
    ui_component_t *list = ui_component_by_id(&g_ctx, g_list_id);
    ui_list_view_data_t *data;

    if (!list || list->type != UI_COMPONENT_LIST_VIEW || !list->component_data) {
        return -1;
    }
    data = (ui_list_view_data_t *)list->component_data;
    if (!data) {
        return -1;
    }
    if (data->list.selected < 0 || data->list.selected >= g_entry_count) {
        return -1;
    }
    return data->list.selected;
}

static void
explorer_rebuild_rows(void)
{
    char line[EXPLORER_NAME_MAX + 2];

    explorer_list_view_clear(g_list_id);
    for (int32_t i = 0; i < g_entry_count; ++i) {
        snprintf(line,
                 sizeof(line),
                 "%s%s",
                 g_entries[i].name,
                 g_entries[i].is_dir ? "/" : "");
        (void)ui_component_list_append(&g_ctx, g_list_id, line);
    }
}

static int32_t
explorer_tree_selected_index(void)
{
    ui_component_t *list = ui_component_by_id(&g_ctx, g_tree_id);
    ui_list_view_data_t *data;

    if (!list || list->type != UI_COMPONENT_LIST_VIEW || !list->component_data) {
        return -1;
    }
    data = (ui_list_view_data_t *)list->component_data;
    if (!data) return -1;
    if (data->list.selected < 0 || data->list.selected >= g_tree_count) return -1;
    return data->list.selected;
}

static void
explorer_tree_append_node(const char *path, const char *name, int32_t depth)
{
    explorer_tree_node_t *node;
    int32_t label_pos = 0;

    if (!path || !name || g_tree_count >= EXPLORER_TREE_MAX) return;
    node = &g_tree_nodes[g_tree_count];
    strncpy(node->path, path, sizeof(node->path) - 1);
    node->path[sizeof(node->path) - 1] = '\0';
    node->depth = depth;

    for (int32_t i = 0; i < depth && label_pos + 2 < (int32_t)sizeof(node->label); ++i) {
        node->label[label_pos++] = ' ';
        node->label[label_pos++] = ' ';
    }
    if (strcmp(path, "/") == 0) {
        node->label[label_pos++] = '/';
    } else {
        size_t name_len = strlen(name);
        if (label_pos + (int32_t)name_len + 1 >= (int32_t)sizeof(node->label)) {
            name_len = (size_t)((int32_t)sizeof(node->label) - label_pos - 2);
        }
        memcpy(node->label + label_pos, name, name_len);
        label_pos += (int32_t)name_len;
        node->label[label_pos++] = '/';
    }
    node->label[label_pos] = '\0';
    g_tree_count++;
}

static void
explorer_tree_walk(const char *path, int32_t depth)
{
    explorer_entry_t dirs[EXPLORER_MAX_ENTRIES];
    int32_t dir_count = 0;

    if (explorer_collect_entries_for_path(path, dirs, EXPLORER_MAX_ENTRIES, 1, &dir_count) != 0) return;
    for (int32_t i = 0; i < dir_count && g_tree_count < EXPLORER_TREE_MAX; ++i) {
        char child_path[EXPLORER_PATH_MAX];
        if (strcmp(path, "/") == 0) snprintf(child_path, sizeof(child_path), "/%s", dirs[i].name);
        else snprintf(child_path, sizeof(child_path), "%s/%s", path, dirs[i].name);
        explorer_tree_append_node(child_path, dirs[i].name, depth);
        if (explorer_path_is_prefix(child_path, g_current_path)) {
            explorer_tree_walk(child_path, depth + 1);
        }
    }
}

static void
explorer_rebuild_tree(void)
{
    ui_component_t *list = ui_component_by_id(&g_ctx, g_tree_id);
    ui_list_view_data_t *data;

    g_tree_count = 0;
    explorer_list_view_clear(g_tree_id);
    explorer_tree_append_node("/", "/", 0);
    explorer_tree_walk("/", 1);
    for (int32_t i = 0; i < g_tree_count; ++i) {
        (void)ui_component_list_append(&g_ctx, g_tree_id, g_tree_nodes[i].label);
    }
    if (!list || list->type != UI_COMPONENT_LIST_VIEW || !list->component_data) return;
    data = (ui_list_view_data_t *)list->component_data;
    if (!data) return;
    data->list.selected = 0;
    for (int32_t i = 0; i < g_tree_count; ++i) {
        if (strcmp(g_tree_nodes[i].path, g_current_path) == 0) {
            data->list.selected = i;
            break;
        }
    }
}

static int
explorer_chdir_root(void)
{
    if (explorer_fs_request(FS_IPC_CHDIR_REQ, 0, 0, 0, 0, NULL) != 0) {
        return -1;
    }
    g_current_path[0] = '/';
    g_current_path[1] = '\0';
    return 0;
}

static int
explorer_chdir_name(const char *name)
{
    int32_t packed[4];

    if (!name || !name[0]) {
        return -1;
    }
    wasmos_ipc_pack_name16(name, packed);
    return explorer_fs_request(FS_IPC_CHDIR_REQ, packed[0], packed[1], packed[2], packed[3], NULL);
}

static void
explorer_path_push(const char *name)
{
    size_t cur_len;
    size_t name_len;

    if (!name) {
        return;
    }
    cur_len = strlen(g_current_path);
    name_len = strlen(name);
    if (cur_len == 1 && g_current_path[0] == '/') {
        if (name_len + 2 >= sizeof(g_current_path)) {
            return;
        }
        memcpy(g_current_path + 1, name, name_len + 1);
        return;
    }
    if (cur_len + name_len + 2 >= sizeof(g_current_path)) {
        return;
    }
    g_current_path[cur_len] = '/';
    memcpy(g_current_path + cur_len + 1, name, name_len + 1);
}

static void
explorer_path_pop(void)
{
    size_t len = strlen(g_current_path);

    if (len <= 1) {
        g_current_path[0] = '/';
        g_current_path[1] = '\0';
        return;
    }
    while (len > 1 && g_current_path[len - 1] != '/') {
        len--;
    }
    if (len <= 1) {
        g_current_path[0] = '/';
        g_current_path[1] = '\0';
        return;
    }
    g_current_path[len - 1] = '\0';
}

static int
explorer_reload(void)
{
    ssize_t nread;
    int32_t start = 0;

    g_entry_count = 0;
    nread = explorer_fs_request_stream(FS_IPC_READDIR_REQ, 0, 0, 0, 0, g_listbuf, sizeof(g_listbuf));
    if (nread < 0) {
        explorer_list_view_clear(g_list_id);
        explorer_update_path_label();
        explorer_set_status("Listing failed");
        ui_mark_dirty(&g_ctx);
        return -1;
    }

    for (int32_t i = 0; i <= (int32_t)nread && g_entry_count < EXPLORER_MAX_ENTRIES; ++i) {
        if (g_listbuf[i] != '\n' && g_listbuf[i] != '\0') {
            continue;
        }
        if (i > start) {
            int32_t raw_len = i - start;
            int32_t is_dir = 0;
            int32_t copy_len = raw_len;
            explorer_entry_t *entry = &g_entries[g_entry_count];

            if (g_listbuf[i - 1] == '/') {
                is_dir = 1;
                copy_len--;
            }
            if (copy_len >= EXPLORER_NAME_MAX) {
                copy_len = EXPLORER_NAME_MAX - 1;
            }
            if (copy_len > 0) {
                memcpy(entry->name, g_listbuf + start, (size_t)copy_len);
            }
            entry->name[copy_len] = '\0';
            entry->is_dir = is_dir;
            if (entry->name[0] != '\0') {
                g_entry_count++;
            }
        }
        start = i + 1;
    }

    explorer_rebuild_rows();
    explorer_rebuild_tree();
    explorer_update_path_label();
    snprintf(g_status_buf, sizeof(g_status_buf), "%d item%s", (int)g_entry_count, g_entry_count == 1 ? "" : "s");
    explorer_set_status(g_status_buf);
    ui_mark_dirty(&g_ctx);
    return 0;
}

static void
explorer_open_selected(ui_context_t *ctx, int32_t id, void *user)
{
    int32_t index;
    explorer_entry_t *entry;

    (void)ctx;
    (void)id;
    (void)user;

    index = explorer_selected_index();
    if (index < 0) {
        explorer_set_status("Select an item first");
        ui_mark_dirty(&g_ctx);
        return;
    }
    entry = &g_entries[index];
    if (entry->is_dir) {
        if (explorer_chdir_name(entry->name) != 0) {
            explorer_set_status("Open directory failed");
            ui_mark_dirty(&g_ctx);
            return;
        }
        explorer_path_push(entry->name);
        (void)explorer_reload();
        return;
    }

    {
        char full_path[EXPLORER_PATH_MAX + EXPLORER_NAME_MAX + 2];
        char status[128];
        struct stat st;

        explorer_build_selected_path(full_path, sizeof(full_path), entry);
        if (explorer_str_ends_with(entry->name, ".wap")) {
            const size_t path_len = strlen(full_path);
            const int32_t pid = (path_len > 0 && path_len <= 240 &&
                                 wasmos_fs_buffer_write((int32_t)(uintptr_t)full_path, (int32_t)path_len, 0) == 0)
                                    ? wasmos_sys_spawn_path_sync(g_proc_endpoint,
                                                                 g_proc_reply_endpoint,
                                                                 (int32_t)path_len,
                                                                 2000,
                                                                 g_ctx.req_id++)
                                    : -1;
            if (pid > 0) {
                snprintf(status, sizeof(status), "Spawned %s (pid %d)", entry->name, (int)pid);
            } else {
                snprintf(status, sizeof(status), "Spawn failed for %s", entry->name);
            }
            explorer_set_status(status);
            ui_mark_dirty(&g_ctx);
            return;
        }
        if (stat(full_path, &st) == 0) {
            snprintf(status, sizeof(status), "File: %s (%u bytes)", entry->name, (unsigned)st.st_size);
        } else {
            snprintf(status, sizeof(status), "File: %s", entry->name);
        }
        explorer_set_status(status);
        ui_mark_dirty(&g_ctx);
    }
}

static void
explorer_activate_entry(ui_context_t *ctx, int32_t component_id, int32_t item_index, void *user)
{
    (void)ctx;
    (void)component_id;
    (void)user;
    if (item_index < 0 || item_index >= g_entry_count) {
        return;
    }
    {
        ui_component_t *list = ui_component_by_id(&g_ctx, g_list_id);
        if (list && list->type == UI_COMPONENT_LIST_VIEW && list->component_data) {
            ((ui_list_view_data_t *)list->component_data)->list.selected = item_index;
        }
    }
    explorer_open_selected(&g_ctx, g_open_button_id, NULL);
}

static void
explorer_open_tree_selected(void)
{
    const int32_t index = explorer_tree_selected_index();
    if (index < 0 || index >= g_tree_count) return;
    if (explorer_chdir_abs(g_tree_nodes[index].path) != 0) {
        explorer_set_status("Open tree folder failed");
        ui_mark_dirty(&g_ctx);
        return;
    }
    strncpy(g_current_path, g_tree_nodes[index].path, sizeof(g_current_path) - 1);
    g_current_path[sizeof(g_current_path) - 1] = '\0';
    (void)explorer_reload();
}

static void
explorer_activate_tree(ui_context_t *ctx, int32_t component_id, int32_t item_index, void *user)
{
    ui_component_t *list;

    (void)ctx;
    (void)component_id;
    (void)user;
    if (item_index < 0 || item_index >= g_tree_count) return;
    list = ui_component_by_id(&g_ctx, g_tree_id);
    if (list && list->type == UI_COMPONENT_LIST_VIEW && list->component_data) {
        ((ui_list_view_data_t *)list->component_data)->list.selected = item_index;
    }
    explorer_open_tree_selected();
}

static void
explorer_secondary_click(ui_context_t *ctx, int32_t component_id, int32_t item_index, void *user)
{
    char status[128];

    (void)ctx;
    (void)component_id;
    (void)user;
    if (item_index < 0 || item_index >= g_entry_count) {
        return;
    }
    snprintf(status, sizeof(status), "Context menu not implemented for %s", g_entries[item_index].name);
    explorer_set_status(status);
    ui_mark_dirty(&g_ctx);
}

static void
explorer_go_up(ui_context_t *ctx, int32_t id, void *user)
{
    (void)ctx;
    (void)id;
    (void)user;

    if (explorer_chdir_name("..") != 0) {
        explorer_set_status("Up failed");
        ui_mark_dirty(&g_ctx);
        return;
    }
    explorer_path_pop();
    (void)explorer_reload();
}

static void
explorer_refresh(ui_context_t *ctx, int32_t id, void *user)
{
    (void)ctx;
    (void)id;
    (void)user;
    (void)explorer_reload();
}

static int
explorer_init_ui(int32_t proc_endpoint, int32_t reply_endpoint)
{
    ui_component_t *root;
    ui_component_t *path_label;
    ui_component_t *status_label;
    ui_component_t *open_button;
    ui_component_t *up_button;
    ui_component_t *refresh_button;
    ui_component_t *toolbar_row;
    ui_component_t *body_row;
    ui_component_t *tree_list;
    ui_component_t *content_panel;
    ui_component_t *list;

    if (ui_init(&g_ctx, proc_endpoint, reply_endpoint, EXPLORER_W, EXPLORER_H) != 0) {
        return -1;
    }
    (void)ui_window_set_title(&g_ctx, "Explorer");

    root = ui_component_by_id(&g_ctx, g_ctx.root_id);
    if (!root) {
        return -1;
    }
    root->bg_color = 0xFF192230u;
    root->padding_px = 10;
    root->gap_px = 8;

    g_path_label_id = ui_component_create_label(&g_ctx);
    g_toolbar_row_id = ui_component_create_row(&g_ctx);
    g_open_button_id = ui_component_create_button(&g_ctx);
    g_up_button_id = ui_component_create_button(&g_ctx);
    g_refresh_button_id = ui_component_create_button(&g_ctx);
    g_body_row_id = ui_component_create_row(&g_ctx);
    g_tree_id = ui_component_create_list_view(&g_ctx);
    g_content_panel_id = ui_component_create_panel(&g_ctx);
    g_list_id = ui_component_create_list_view(&g_ctx);
    g_status_label_id = ui_component_create_label(&g_ctx);
    if (g_path_label_id < 0 || g_toolbar_row_id < 0 || g_open_button_id < 0 ||
        g_up_button_id < 0 || g_refresh_button_id < 0 || g_body_row_id < 0 ||
        g_tree_id < 0 || g_content_panel_id < 0 || g_list_id < 0 || g_status_label_id < 0) {
        return -1;
    }

    path_label = ui_component_by_id(&g_ctx, g_path_label_id);
    toolbar_row = ui_component_by_id(&g_ctx, g_toolbar_row_id);
    open_button = ui_component_by_id(&g_ctx, g_open_button_id);
    up_button = ui_component_by_id(&g_ctx, g_up_button_id);
    refresh_button = ui_component_by_id(&g_ctx, g_refresh_button_id);
    body_row = ui_component_by_id(&g_ctx, g_body_row_id);
    tree_list = ui_component_by_id(&g_ctx, g_tree_id);
    content_panel = ui_component_by_id(&g_ctx, g_content_panel_id);
    list = ui_component_by_id(&g_ctx, g_list_id);
    status_label = ui_component_by_id(&g_ctx, g_status_label_id);
    if (!path_label || !toolbar_row || !open_button || !up_button || !refresh_button ||
        !body_row || !tree_list || !content_panel || !list || !status_label) {
        return -1;
    }

    path_label->preferred_h = EXPLORER_PATH_H;
    path_label->bg_color = 0xFF233146u;
    path_label->padding_px = 6;
    path_label->fg_color = 0xFFFFFFFFu;

    toolbar_row->preferred_h = EXPLORER_TOOLBAR_H;
    toolbar_row->bg_color = 0x00000000u;
    toolbar_row->border_px = 0;
    toolbar_row->padding_px = 0;
    toolbar_row->gap_px = 8;

    open_button->preferred_h = 132;
    open_button->bg_color = 0xFF2B5E72u;
    open_button->fg_color = 0xFFFFFFFFu;
    open_button->clickable = 1;

    up_button->preferred_h = 92;
    up_button->bg_color = 0xFF31475Fu;
    up_button->fg_color = 0xFFFFFFFFu;
    up_button->clickable = 1;

    refresh_button->preferred_h = 108;
    refresh_button->bg_color = 0xFF31475Fu;
    refresh_button->fg_color = 0xFFFFFFFFu;
    refresh_button->clickable = 1;

    body_row->preferred_h = explorer_list_height();
    body_row->bg_color = 0x00000000u;
    body_row->border_px = 0;
    body_row->padding_px = 0;
    body_row->gap_px = 10;

    tree_list->preferred_h = EXPLORER_SIDEBAR_W;
    tree_list->bg_color = 0xFF16212Eu;
    tree_list->border_color = 0xFF58708Du;
    tree_list->border_px = 1;
    tree_list->padding_px = 4;

    content_panel->preferred_h = 0;
    content_panel->bg_color = 0x00000000u;
    content_panel->border_px = 0;
    content_panel->padding_px = 0;
    content_panel->gap_px = 0;

    list->preferred_h = explorer_list_height() - 8;
    list->bg_color = 0xFF1B2535u;
    list->border_color = 0xFF58708Du;
    list->border_px = 1;
    list->padding_px = 4;

    status_label->preferred_h = EXPLORER_STATUS_H;
    status_label->bg_color = 0xFF1F2B3Eu;
    status_label->padding_px = 6;
    status_label->fg_color = 0xFFBFD2E6u;

    ui_component_set_text(&g_ctx, g_open_button_id, "Open Selected");
    ui_component_set_text(&g_ctx, g_up_button_id, "Up");
    ui_component_set_text(&g_ctx, g_refresh_button_id, "Refresh");

    ui_component_set_button_action(&g_ctx, g_open_button_id, explorer_open_selected, NULL);
    ui_component_set_button_action(&g_ctx, g_up_button_id, explorer_go_up, NULL);
    ui_component_set_button_action(&g_ctx, g_refresh_button_id, explorer_refresh, NULL);
    ui_component_set_list_view_activate_action(&g_ctx, g_tree_id, explorer_activate_tree, NULL);
    ui_component_set_list_view_activate_action(&g_ctx, g_list_id, explorer_activate_entry, NULL);
    ui_component_set_list_view_secondary_click_action(&g_ctx, g_list_id, explorer_secondary_click, NULL);

    (void)ui_component_append_child(&g_ctx, g_ctx.root_id, g_path_label_id);
    (void)ui_component_append_child(&g_ctx, g_ctx.root_id, g_toolbar_row_id);
    (void)ui_component_append_child(&g_ctx, g_toolbar_row_id, g_open_button_id);
    (void)ui_component_append_child(&g_ctx, g_toolbar_row_id, g_up_button_id);
    (void)ui_component_append_child(&g_ctx, g_toolbar_row_id, g_refresh_button_id);
    (void)ui_component_append_child(&g_ctx, g_ctx.root_id, g_body_row_id);
    (void)ui_component_append_child(&g_ctx, g_body_row_id, g_tree_id);
    (void)ui_component_append_child(&g_ctx, g_body_row_id, g_content_panel_id);
    (void)ui_component_append_child(&g_ctx, g_content_panel_id, g_list_id);
    (void)ui_component_append_child(&g_ctx, g_ctx.root_id, g_status_label_id);
    return 0;
}

int
main(int argc, char **argv)
{
    const int32_t proc_endpoint = wasmos_startup_arg(0);
    const int32_t reply_endpoint = wasmos_ipc_create_endpoint();
    int32_t have_root_cwd = 0;
    g_fs_reply_endpoint = wasmos_ipc_create_endpoint();
    g_proc_reply_endpoint = wasmos_ipc_create_endpoint();

    (void)argc;
    (void)argv;
    g_proc_endpoint = proc_endpoint;

    if (proc_endpoint <= 0 || reply_endpoint < 0 || g_fs_reply_endpoint < 0 || g_proc_reply_endpoint < 0) {
        return 1;
    }
    if (explorer_init_ui(proc_endpoint, reply_endpoint) != 0) {
        ui_destroy(&g_ctx);
        return 1;
    }
    if (explorer_chdir_root() != 0) {
        explorer_set_status("FS unavailable");
        ui_mark_dirty(&g_ctx);
    } else {
        have_root_cwd = 1;
    }
    if (ui_loop_drain(&g_ctx) != 0) {
        ui_destroy(&g_ctx);
        return 1;
    }
    if (have_root_cwd) {
        (void)explorer_reload();
        if (ui_loop_drain(&g_ctx) != 0) {
            ui_destroy(&g_ctx);
            return 1;
        }
    }

    while (!g_ctx.close_requested) {
        wasmos_ipc_message_t msg;

        if (ui_send_gfx_raw(g_ctx.gfx_endpoint,
                            g_ctx.reply_endpoint,
                            g_ctx.req_id++,
                            GFX_IPC_POLL_EVENT,
                            0,
                            0,
                            0,
                            0,
                            &msg) == 0) {
            (void)ui_loop_handle_ipc(&g_ctx, &msg);
        }
        if (ui_loop_drain(&g_ctx) != 0) {
            break;
        }
    }

    ui_destroy(&g_ctx);
    return 0;
}
