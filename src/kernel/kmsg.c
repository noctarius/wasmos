#include "kmsg.h"

#include "wasmos_status.h"

void kmsg_append(kmsg_t* k, const char* s) {
    if (!s) {
        return;
    }
    for (const char* p = s; *p; ++p) {
        k->data[k->next & (KMSG_SIZE - 1u)] = (uint8_t)*p;
        k->next++;
    }
}

uint64_t kmsg_first(const kmsg_t* k) {
    return k->next > KMSG_SIZE ? k->next - KMSG_SIZE : 0;
}

uint32_t kmsg_size(const kmsg_t* k) {
    return (uint32_t)(k->next - kmsg_first(k));
}

void kmsg_copy(const kmsg_t* k, uint8_t* dst, uint32_t offset, uint32_t len) {
    uint32_t size = kmsg_size(k);
    if (!dst || offset >= size) {
        return;
    }
    if (len > size - offset) {
        len = size - offset;
    }
    uint64_t start = kmsg_first(k) + offset;
    for (uint32_t i = 0; i < len; ++i) {
        dst[i] = k->data[(start + i) & (KMSG_SIZE - 1u)];
    }
}

int32_t kmsg_read(const kmsg_t* k, uint64_t* seq, uint8_t* dst, uint32_t len) {
    if (!seq || !dst || *seq > k->next) {
        return WASMOS_INVAL;
    }
    uint64_t first = kmsg_first(k);
    uint64_t start = *seq < first ? first : *seq;
    uint64_t avail = k->next - start;
    uint32_t n = avail < len ? (uint32_t)avail : len;
    if (n > (uint32_t)INT32_MAX) {
        n = (uint32_t)INT32_MAX;
    }
    for (uint32_t i = 0; i < n; ++i) {
        dst[i] = k->data[(start + i) & (KMSG_SIZE - 1u)];
    }
    *seq = start;
    return (int32_t)n;
}

/* Ends `sub` if that exact (context, endpoint) pair is still subscribed: a
 * context that re-subscribed meanwhile keeps its new endpoint. Lock held. */
static void kmsg_notify_drop_locked(kmsg_notify_t* n, const kmsg_subscriber_t* sub) {
    for (uint32_t i = 0; i < KMSG_SUBSCRIBER_MAX; ++i) {
        if (n->subs[i].context_id == sub->context_id && n->subs[i].endpoint == sub->endpoint) {
            n->subs[i].context_id = 0;
            n->subs[i].endpoint = 0;
            n->subs[i].owed = 0;
            return;
        }
    }
}

static int32_t kmsg_notify_subscribe_locked(kmsg_notify_t* n, uint32_t context_id,
                                            uint32_t endpoint) {
    kmsg_subscriber_t* free_slot = 0;
    for (uint32_t i = 0; i < KMSG_SUBSCRIBER_MAX; ++i) {
        if (n->subs[i].context_id == context_id) {
            n->subs[i].endpoint = endpoint;
            return 0;
        }
        if (n->subs[i].context_id == 0 && !free_slot) {
            free_slot = &n->subs[i];
        }
    }
    if (!free_slot) {
        return WASMOS_ERR_KERNEL_NO_SUBSCRIBER_SLOT;
    }
    free_slot->context_id = context_id;
    free_slot->endpoint = endpoint;
    free_slot->owed = 0;
    return 0;
}

/* Ends every subscription whose context no longer owns its endpoint. The
 * table is copied under the lock and checked with it released; a drop matches
 * the exact pair, so a slot re-subscribed in between is left alone. */
static void kmsg_notify_prune(kmsg_notify_t* n, const kmsg_notify_env_t* env) {
    kmsg_subscriber_t subs[KMSG_SUBSCRIBER_MAX];
    env->lock(env->user);
    for (uint32_t i = 0; i < KMSG_SUBSCRIBER_MAX; ++i) {
        subs[i] = n->subs[i];
    }
    env->unlock(env->user);
    for (uint32_t i = 0; i < KMSG_SUBSCRIBER_MAX; ++i) {
        if (subs[i].context_id != 0 &&
            !env->alive(env->user, subs[i].context_id, subs[i].endpoint)) {
            env->lock(env->user);
            kmsg_notify_drop_locked(n, &subs[i]);
            env->unlock(env->user);
        }
    }
}

int32_t kmsg_notify_subscribe(kmsg_notify_t* n, const kmsg_notify_env_t* env, uint32_t context_id,
                              uint32_t endpoint) {
    if (context_id == 0 || endpoint == 0) {
        return WASMOS_INVAL;
    }
    env->lock(env->user);
    int32_t rc = kmsg_notify_subscribe_locked(n, context_id, endpoint);
    env->unlock(env->user);
    if (rc != WASMOS_ERR_KERNEL_NO_SUBSCRIBER_SLOT) {
        return rc;
    }
    /* Delivery drops exited subscribers, but delivery only follows an append,
     * so a full table may hold none that is alive. */
    kmsg_notify_prune(n, env);
    env->lock(env->user);
    rc = kmsg_notify_subscribe_locked(n, context_id, endpoint);
    env->unlock(env->user);
    return rc;
}

void kmsg_notify_mark(kmsg_notify_t* n) {
    for (uint32_t i = 0; i < KMSG_SUBSCRIBER_MAX; ++i) {
        if (n->subs[i].context_id != 0) {
            n->subs[i].owed = 1;
            n->pending = 1;
        }
    }
}

void kmsg_notify_deliver(kmsg_notify_t* n, const kmsg_notify_env_t* env) {
    kmsg_subscriber_t subs[KMSG_SUBSCRIBER_MAX];
    uint32_t count = 0;
    env->lock(env->user);
    if (n->pending) {
        n->pending = 0;
        for (uint32_t i = 0; i < KMSG_SUBSCRIBER_MAX; ++i) {
            if (n->subs[i].context_id != 0 && n->subs[i].owed) {
                n->subs[i].owed = 0;
                subs[count++] = n->subs[i];
            }
        }
    }
    env->unlock(env->user);
    for (uint32_t i = 0; i < count; ++i) {
        if (!env->alive(env->user, subs[i].context_id, subs[i].endpoint)) {
            env->lock(env->user);
            kmsg_notify_drop_locked(n, &subs[i]);
            env->unlock(env->user);
            continue;
        }
        if (env->send(env->user, subs[i].endpoint) != 0) {
            /* Not queued: the subscriber may be parked on a future that only
             * this notify resolves, so it stays owed and the next delivery
             * retries it. A subscription renewed meanwhile is owed anyway. */
            env->lock(env->user);
            for (uint32_t j = 0; j < KMSG_SUBSCRIBER_MAX; ++j) {
                if (n->subs[j].context_id == subs[i].context_id &&
                    n->subs[j].endpoint == subs[i].endpoint) {
                    n->subs[j].owed = 1;
                    n->pending = 1;
                }
            }
            env->unlock(env->user);
        }
    }
}
