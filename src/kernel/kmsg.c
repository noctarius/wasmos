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

int32_t kmsg_notify_subscribe(kmsg_notify_t* n, uint32_t context_id, uint32_t endpoint) {
    if (context_id == 0 || endpoint == 0) {
        return WASMOS_INVAL;
    }
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
    return 0;
}

void kmsg_notify_mark(kmsg_notify_t* n) {
    for (uint32_t i = 0; i < KMSG_SUBSCRIBER_MAX; ++i) {
        if (n->subs[i].context_id != 0) {
            n->pending = 1;
            return;
        }
    }
}

uint32_t kmsg_notify_take(kmsg_notify_t* n, kmsg_subscriber_t out[KMSG_SUBSCRIBER_MAX]) {
    if (!n->pending) {
        return 0;
    }
    n->pending = 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < KMSG_SUBSCRIBER_MAX; ++i) {
        if (n->subs[i].context_id != 0) {
            out[count++] = n->subs[i];
        }
    }
    return count;
}

void kmsg_notify_drop(kmsg_notify_t* n, const kmsg_subscriber_t* sub) {
    for (uint32_t i = 0; i < KMSG_SUBSCRIBER_MAX; ++i) {
        if (n->subs[i].context_id == sub->context_id && n->subs[i].endpoint == sub->endpoint) {
            n->subs[i].context_id = 0;
            n->subs[i].endpoint = 0;
            return;
        }
    }
}
