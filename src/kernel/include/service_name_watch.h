#ifndef WASMOS_SERVICE_NAME_WATCH_H
#define WASMOS_SERVICE_NAME_WATCH_H

/* service_name_watch - subscribers to the binding of a service NAME, beside the
 * PM name table (docs/architecture/09-process-and-ipc.md). The PM reports each
 * bind through service_name_watch_bound(); every live subscriber of that name
 * receives one SVC_NAME_EVENT_BIND through the injected event sink.
 *
 * A subscription is keyed by (notify_endpoint, cookie). The cookie is the
 * subscriber's own tag and is echoed in every event, so one endpoint can watch
 * several names. Environment coupling (event delivery, owner liveness) is
 * injected via callbacks; the table is a dynamic list_t. */

#include <stdint.h>

#include "../../../abi/generated/c/wasmos_constants.h"

#define SVC_WATCH_NAME_MAX 36u /* incl. NUL; keep == WASMOS_SVC_NAME_MAX */

/* Delivers one event. The PM pushes an SVC_IPC_NAME_EVENT to notify_endpoint. */
typedef void (*service_name_watch_event_fn)(void* user, uint32_t notify_endpoint, uint32_t event,
                                            uint32_t cookie, uint32_t endpoint, uint32_t pid);

/* Nonzero iff owner_ctx still has a live process. */
typedef int (*service_name_watch_alive_fn)(void* user, uint32_t owner_ctx);

/* Clear every subscription and the event sink. No events are fired. */
void service_name_watch_reset(void);

/* Install the callback used to deliver BIND events, replacing any previous one;
 * NULL stops delivery (subscriptions are still recorded). `user` is borrowed and
 * passed through unchanged. The sink runs synchronously from
 * service_name_watch_bound, so it must not block or re-enter this module. */
void service_name_watch_set_event_sink(service_name_watch_event_fn fn, void* user);

/* The key belongs to whoever owns notify_endpoint, and the CALLER must have
 * verified that owner_ctx owns it before calling either function below. An
 * existing entry under the same key with a different owner is therefore stale --
 * left by a dead previous holder of that endpoint id -- and is taken over, never
 * refused: refusing it would let a dead process block the endpoint's new owner. */

/* Subscribe (notify_endpoint, cookie) to the binding of `name`. Subscribing a
 * key that exists re-targets it at `name`. Does NOT replay a binding that
 * already exists -- the caller looks the name up after subscribing.
 *
 * Returns 0, or negative for a NULL/empty/over-long name (refused, never
 * truncated, so two names cannot collide on a shared prefix) or a failed
 * allocation. */
int service_name_watch_subscribe(const char* name, uint32_t notify_endpoint, uint32_t cookie,
                                 uint32_t owner_ctx);

/* Drop the subscription (notify_endpoint, cookie). Idempotent: a key that is not
 * subscribed is not an error. */
void service_name_watch_unsubscribe(uint32_t notify_endpoint, uint32_t cookie);

/* Report that `name` is now bound to (endpoint, pid). Emits SVC_NAME_EVENT_BIND
 * to every subscriber of `name` whose owner is alive per `alive`, and DROPS each
 * subscriber whose owner is not -- subscriptions of dead processes are reclaimed
 * here, on the path that would address them, instead of by a sweep on every PM
 * dispatch. A NULL `alive` treats every subscriber as alive. */
void service_name_watch_bound(const char* name, uint32_t endpoint, uint32_t pid,
                              service_name_watch_alive_fn alive, void* user);

/* Number of recorded subscriptions, dead owners included until a bind of their
 * name reclaims them. */
uint32_t service_name_watch_count(void);

#endif
