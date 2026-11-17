/* service_name_watch.c - see service_name_watch.h. */

#include "service_name_watch.h"

#include "list.h"
#include "string.h"

typedef struct {
    char name[SVC_WATCH_NAME_MAX];
    uint32_t notify_endpoint;
    uint32_t cookie;
    uint32_t owner_ctx;
} watch_t;

static list_t g_watches;
static uint8_t g_inited;
static service_name_watch_event_fn g_event_fn;
static void* g_event_user;

static void snw_ensure_init(void) {
    if (g_inited) {
        return;
    }
    list_init(&g_watches, (uint32_t)sizeof(watch_t), LIST_IMPL_LINKED, 0);
    g_inited = 1;
}

/* Copy a NUL-terminated name bounded by SVC_WATCH_NAME_MAX. Returns 0, or -1 if
 * src is NULL, empty, or does not fit. */
static int snw_copy_name(char* dst, const char* src) {
    if (src == 0 || src[0] == '\0') {
        return -1;
    }
    return str_copy_bytes(dst, SVC_WATCH_NAME_MAX, (const uint8_t*)src, strlen(src));
}

static watch_t* snw_find(uint32_t notify_endpoint, uint32_t cookie) {
    list_iter_t it;
    watch_t* w;
    for (w = (watch_t*)list_first(&g_watches, &it); w; w = (watch_t*)list_next(&it)) {
        if (w->notify_endpoint == notify_endpoint && w->cookie == cookie) {
            return w;
        }
    }
    return 0;
}

void service_name_watch_reset(void) {
    list_iter_t it;
    void* e;
    snw_ensure_init();
    while ((e = list_first(&g_watches, &it)) != 0) {
        list_remove(&g_watches, e);
    }
    g_event_fn = 0;
    g_event_user = 0;
}

void service_name_watch_set_event_sink(service_name_watch_event_fn fn, void* user) {
    g_event_fn = fn;
    g_event_user = user;
}

int service_name_watch_subscribe(const char* name, uint32_t notify_endpoint, uint32_t cookie,
                                 uint32_t owner_ctx) {
    char norm[SVC_WATCH_NAME_MAX];
    watch_t* w;

    snw_ensure_init();
    if (snw_copy_name(norm, name) != 0) {
        return -1;
    }
    w = snw_find(notify_endpoint, cookie);
    if (!w) {
        w = (watch_t*)list_alloc(&g_watches);
        if (!w) {
            return -1;
        }
        w->notify_endpoint = notify_endpoint;
        w->cookie = cookie;
    }
    (void)snw_copy_name(w->name, norm);
    w->owner_ctx = owner_ctx;
    return 0;
}

void service_name_watch_unsubscribe(uint32_t notify_endpoint, uint32_t cookie) {
    watch_t* w;
    snw_ensure_init();
    w = snw_find(notify_endpoint, cookie);
    if (w) {
        list_remove(&g_watches, w);
    }
}

/* Two passes: dead subscribers of `name` are removed first, one per list walk
 * (re-fetching from the head each time so no live iterator spans a remove), and
 * only then are events emitted, so the emit walk never sees a removal. */
void service_name_watch_bound(const char* name, uint32_t endpoint, uint32_t pid,
                              service_name_watch_alive_fn alive, void* user) {
    list_iter_t it;
    watch_t* w;

    snw_ensure_init();
    if (name == 0 || name[0] == '\0') {
        return;
    }
    if (alive != 0) {
        for (;;) {
            watch_t* dead = 0;
            for (w = (watch_t*)list_first(&g_watches, &it); w; w = (watch_t*)list_next(&it)) {
                if (strcmp(w->name, name) == 0 && !alive(user, w->owner_ctx)) {
                    dead = w;
                    break;
                }
            }
            if (!dead) {
                break;
            }
            list_remove(&g_watches, dead);
        }
    }
    if (g_event_fn == 0) {
        return;
    }
    for (w = (watch_t*)list_first(&g_watches, &it); w; w = (watch_t*)list_next(&it)) {
        if (strcmp(w->name, name) == 0) {
            g_event_fn(
                g_event_user, w->notify_endpoint, SVC_NAME_EVENT_BIND, w->cookie, endpoint, pid);
        }
    }
}

uint32_t service_name_watch_count(void) {
    list_iter_t it;
    uint32_t n = 0;
    snw_ensure_init();
    for (void* e = list_first(&g_watches, &it); e; e = list_next(&it)) {
        n++;
    }
    return n;
}
