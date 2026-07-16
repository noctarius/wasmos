/* service_class_registry.c - see service_class_registry.h. */

#include "service_class_registry.h"

#include "list.h"
#include "string.h"

typedef struct {
    char     class_name[SVC_CLASS_NAME_MAX];
    uint32_t instance;
    uint32_t endpoint;
    uint32_t owner_ctx;
    uint32_t pid;
} provider_t;

typedef struct {
    char     class_name[SVC_CLASS_NAME_MAX];
    uint32_t notify_endpoint;
    uint32_t owner_ctx;
} sub_t;

static list_t g_providers;
static list_t g_subs;
static uint8_t g_inited;
static service_class_event_fn g_event_fn;
static void *g_event_user;

static int
scr_streq(const char *a, const char *b)
{
    uint32_t i = 0;
    for (; a[i] != '\0' && b[i] != '\0'; ++i) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return a[i] == b[i];
}

/* Copy a NUL-terminated class name bounded by SVC_CLASS_NAME_MAX. Returns 0, or
 * -1 if src is empty or does not fit. */
static int
scr_copy_class(char *dst, const char *src)
{
    if (src == 0 || src[0] == '\0') {
        return -1;
    }
    /* Reject rather than truncate an over-long class name. */
    return str_copy_bytes(dst, SVC_CLASS_NAME_MAX, (const uint8_t *)src, strlen(src));
}

static void
scr_ensure_init(void)
{
    if (g_inited) {
        return;
    }
    list_init(&g_providers, (uint32_t)sizeof(provider_t), LIST_IMPL_LINKED, 0);
    list_init(&g_subs, (uint32_t)sizeof(sub_t), LIST_IMPL_LINKED, 0);
    g_inited = 1;
}

/* Remove every node from `list` (re-fetching first each time so no live
 * iterator spans a remove). */
static void
scr_list_clear(list_t *list)
{
    list_iter_t it;
    void *e;
    while ((e = list_first(list, &it)) != 0) {
        list_remove(list, e);
    }
}

void
service_class_registry_reset(void)
{
    scr_ensure_init();
    scr_list_clear(&g_providers);
    scr_list_clear(&g_subs);
    g_event_fn = 0;
    g_event_user = 0;
}

void
service_class_registry_set_event_sink(service_class_event_fn fn, void *user)
{
    g_event_fn = fn;
    g_event_user = user;
}

static void
scr_emit(uint32_t event, const char *class_name, uint32_t instance,
         uint32_t endpoint, uint32_t pid)
{
    list_iter_t it;
    sub_t *s;
    if (g_event_fn == 0) {
        return;
    }
    for (s = (sub_t *)list_first(&g_subs, &it); s; s = (sub_t *)list_next(&it)) {
        if (scr_streq(s->class_name, class_name)) {
            g_event_fn(g_event_user, s->notify_endpoint, event, class_name,
                       instance, endpoint, pid);
        }
    }
}

int
service_class_registry_add(const char *class_name, uint32_t instance,
                           uint32_t endpoint, uint32_t owner_ctx, uint32_t pid)
{
    char norm[SVC_CLASS_NAME_MAX];
    list_iter_t it;
    provider_t *p;

    scr_ensure_init();
    if (scr_copy_class(norm, class_name) != 0) {
        return -1;
    }
    for (p = (provider_t *)list_first(&g_providers, &it); p;
         p = (provider_t *)list_next(&it)) {
        if (p->instance == instance && scr_streq(p->class_name, norm)) {
            if (p->owner_ctx != owner_ctx) {
                return -1; /* another owner holds this (class, instance) */
            }
            p->endpoint = endpoint;
            p->pid = pid;
            return 0;
        }
    }
    p = (provider_t *)list_alloc(&g_providers);
    if (!p) {
        return -1;
    }
    (void)scr_copy_class(p->class_name, norm);
    p->instance = instance;
    p->endpoint = endpoint;
    p->owner_ctx = owner_ctx;
    p->pid = pid;
    scr_emit(SVC_CLASS_EVENT_ADD, norm, instance, endpoint, pid);
    return 0;
}

uint32_t
service_class_registry_lookup(const char *class_name,
                              service_class_provider_t *out, uint32_t max)
{
    uint32_t total = 0;
    uint32_t last = 0;
    uint8_t have_last = 0;

    scr_ensure_init();
    if (class_name == 0 || class_name[0] == '\0') {
        return 0;
    }
    /* Emit in ascending instance order: each pass picks the smallest instance
     * strictly greater than the previous one. */
    for (;;) {
        list_iter_t it;
        provider_t *p;
        provider_t *best = 0;
        for (p = (provider_t *)list_first(&g_providers, &it); p;
             p = (provider_t *)list_next(&it)) {
            if (!scr_streq(p->class_name, class_name)) {
                continue;
            }
            if (have_last && p->instance <= last) {
                continue;
            }
            if (!best || p->instance < best->instance) {
                best = p;
            }
        }
        if (!best) {
            break;
        }
        if (out && total < max) {
            out[total].instance = best->instance;
            out[total].endpoint = best->endpoint;
            out[total].pid = best->pid;
        }
        last = best->instance;
        have_last = 1;
        total++;
    }
    return total;
}

int
service_class_registry_subscribe(const char *class_name,
                                 uint32_t notify_endpoint, uint32_t owner_ctx)
{
    char norm[SVC_CLASS_NAME_MAX];
    list_iter_t it;
    sub_t *s;

    scr_ensure_init();
    if (scr_copy_class(norm, class_name) != 0) {
        return -1;
    }
    for (s = (sub_t *)list_first(&g_subs, &it); s; s = (sub_t *)list_next(&it)) {
        if (s->notify_endpoint == notify_endpoint && scr_streq(s->class_name, norm)) {
            return 0;
        }
    }
    s = (sub_t *)list_alloc(&g_subs);
    if (!s) {
        return -1;
    }
    (void)scr_copy_class(s->class_name, norm);
    s->notify_endpoint = notify_endpoint;
    s->owner_ctx = owner_ctx;
    return 0;
}

/* Find one entry in `list` whose owner is not alive; return it or NULL. */
static void *
scr_find_dead(list_t *list, uint32_t owner_off, service_class_alive_fn alive,
              void *user)
{
    list_iter_t it;
    uint8_t *e;
    for (e = (uint8_t *)list_first(list, &it); e; e = (uint8_t *)list_next(&it)) {
        uint32_t owner = *(uint32_t *)(e + owner_off);
        if (!alive(user, owner)) {
            return e;
        }
    }
    return 0;
}

void
service_class_registry_reap_dead(service_class_alive_fn alive, void *user)
{
    sub_t *dead_sub;
    provider_t *dead_prov;

    if (alive == 0) {
        return;
    }
    scr_ensure_init();
    /* Drop dead subscribers first so no REMOVE targets a dead endpoint. */
    while ((dead_sub = (sub_t *)scr_find_dead(&g_subs,
               (uint32_t)__builtin_offsetof(sub_t, owner_ctx), alive, user)) != 0) {
        list_remove(&g_subs, dead_sub);
    }
    while ((dead_prov = (provider_t *)scr_find_dead(&g_providers,
               (uint32_t)__builtin_offsetof(provider_t, owner_ctx), alive, user)) != 0) {
        scr_emit(SVC_CLASS_EVENT_REMOVE, dead_prov->class_name,
                 dead_prov->instance, dead_prov->endpoint, dead_prov->pid);
        list_remove(&g_providers, dead_prov);
    }
}