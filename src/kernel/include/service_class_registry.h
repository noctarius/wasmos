#ifndef WASMOS_SERVICE_CLASS_REGISTRY_H
#define WASMOS_SERVICE_CLASS_REGISTRY_H

/* service_class_registry - (class, instance) -> {endpoint, owner, pid} table
 * plus per-class subscribers, beside the PM name table
 * (docs/architecture/09-process-and-ipc.md). Environment coupling (IPC event
 * delivery, owner liveness) is injected via callbacks; the tables are dynamic
 * list_t. */

#include <stdint.h>

#define SVC_CLASS_NAME_MAX 16u /* incl. NUL; keep == WASMOS_SVC_CLASS_MAX */

/* Existence-event kinds. Keep in sync with SVC_CLASS_EVENT_* in
 * src/drivers/include/wasmos_driver_abi.h. */
#define SVC_CLASS_EVENT_ADD 1u
#define SVC_CLASS_EVENT_REMOVE 2u

/* Layout matches the wire svc_class_entry_t. */
typedef struct {
    uint32_t instance;
    uint32_t endpoint;
    uint32_t pid;
} service_class_provider_t;

/* Called when a provider is added to / removed from a class with subscribers.
 * The PM pushes an SVC_IPC_CLASS_EVENT to notify_endpoint. */
typedef void (*service_class_event_fn)(void* user, uint32_t notify_endpoint, uint32_t event,
                                       const char* class_name, uint32_t instance, uint32_t endpoint,
                                       uint32_t pid);

/* Nonzero iff owner_ctx still has a live process. */
typedef int (*service_class_alive_fn)(void* user, uint32_t owner_ctx);

/* Clear all providers, subscribers, and the event sink. No events are fired for
 * the entries it drops. */
void service_class_registry_reset(void);

/* Install the callback used to deliver ADD/REMOVE events, replacing any
 * previous one; `fn` may be NULL to stop delivering them (registrations still
 * proceed). `user` is passed through unchanged and is borrowed -- it must
 * outlive the sink. The sink is invoked synchronously from add / reap_dead, so
 * it must not block or re-enter the registry. */
void service_class_registry_set_event_sink(service_class_event_fn fn, void* user);

/* Register a provider under (class_name, instance). Re-registration by the same
 * owner updates endpoint/pid without an event; a new (class, instance) fires
 * SVC_CLASS_EVENT_ADD. Returns 0, or negative on bad arguments, allocation
 * failure, or a (class, instance) held by another owner. */
int service_class_registry_add(const char* class_name, uint32_t instance, uint32_t endpoint,
                               uint32_t owner_ctx, uint32_t pid);

/* Write matching providers to out[0..max) in ascending instance order; returns
 * the total match count (may exceed max). */
uint32_t service_class_registry_lookup(const char* class_name, service_class_provider_t* out,
                                       uint32_t max);

/* Subscribe notify_endpoint to class_name events. Idempotent per
 * (class_name, notify_endpoint). Returns 0 or negative on error. */
int service_class_registry_subscribe(const char* class_name, uint32_t notify_endpoint,
                                     uint32_t owner_ctx);

/* Purge providers and subscribers whose owner is not alive per `alive`, firing
 * SVC_CLASS_EVENT_REMOVE to surviving subscribers for each purged provider.
 * Dead subscribers are dropped before any REMOVE is emitted. */
void service_class_registry_reap_dead(service_class_alive_fn alive, void* user);

#endif /* WASMOS_SERVICE_CLASS_REGISTRY_H */