/* port.c - lwIP NO_SYS port hooks for the wasmos native net-stack service.
 *
 * COMPILE MILESTONE: only the minimum hooks lwIP references under NO_SYS=1 are
 * provided:
 *   - sys_now(): monotonic milliseconds.
 *   - lwip_port_rand(): PRNG for LWIP_RAND().
 *   - atoi(): needed by lwIP netif.c (see note below).
 *
 * SYS_LIGHTWEIGHT_PROT=0, so no sys_arch_protect/unprotect is needed.
 * lwip_htons/lwip_htonl are provided by lwIP itself (def.c). No netif glue,
 * kernel process, or driver wiring lives here yet.
 *
 * Unlike the earlier kernel-image variant, this is a native service (.wap): it
 * CANNOT call kernel symbols (timer_ticks(), etc.) directly. Time is obtained
 * through the wasmos_driver_api_t table handed to the service entry. net_stack.c
 * stashes that pointer in net_stack_api() so port.c can reach it here.
 */
#include <stdint.h>
#include <stddef.h>

#include "lwip/sys.h"

#include "wasmos_native_driver.h"

/* Provided by net_stack.c: the api table captured at initialize() time.
 * May be NULL before initialize() runs; callers must tolerate that. */
wasmos_driver_api_t *net_stack_api(void);

/* Return monotonic time in milliseconds.
 *
 * The native driver ABI exposes only sched_ticks() (a monotonic scheduler tick
 * counter) with no published tick frequency and no ticks->ms converter at this
 * layer. lwIP only requires sys_now() to be monotonic and roughly in ms for
 * NO_SYS timeout bookkeeping, which is not exercised in this compile milestone
 * (no netif, no timeouts registered yet). We therefore return the raw tick
 * count as a monotonically increasing value, falling back to a local counter if
 * the api/hook is unavailable.
 *
 * TODO(net_stack): real ms clock via libsys_native — convert sched_ticks() with
 * the actual timer frequency (or add a native ms hook) when the netif/ICMP step
 * lands and lwIP timeouts start mattering. */
u32_t sys_now(void) {
    wasmos_driver_api_t *api = net_stack_api();
    if (api != NULL && api->sched_ticks != NULL) {
        return (u32_t)api->sched_ticks();
    }
    static u32_t fallback = 0u;
    return ++fallback;
}

/* Minimal xorshift PRNG for LWIP_RAND(). Seeded from sched_ticks() when
 * available. Sufficient for TCP ISN / ephemeral port selection; not
 * cryptographically secure.
 * TODO(net_stack): replace with a stronger entropy source once the net-stack
 * service is wired up. */
uint32_t lwip_port_rand(void) {
    static uint32_t state = 0u;
    if (state == 0u) {
        wasmos_driver_api_t *api = net_stack_api();
        uint32_t seed = 0x2545F491u;
        if (api != NULL && api->sched_ticks != NULL) {
            seed ^= (uint32_t)api->sched_ticks();
        }
        state = seed | 1u;
    }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

/* atoi(): lwIP netif.c uses it to parse a numeric netif name suffix. The libc
 * objects this service links (string.c, ctype.c) do not include atoi (it lives
 * in stdlib.c, which is not linked), so provide a minimal implementation here
 * to satisfy the link without editing libs/lwip or pulling in more of libc. */
int atoi(const char *nptr) {
    int sign = 1;
    int value = 0;
    if (nptr == NULL) {
        return 0;
    }
    while (*nptr == ' ' || *nptr == '\t' || *nptr == '\n' ||
           *nptr == '\r' || *nptr == '\f' || *nptr == '\v') {
        nptr++;
    }
    if (*nptr == '+' || *nptr == '-') {
        if (*nptr == '-') {
            sign = -1;
        }
        nptr++;
    }
    while (*nptr >= '0' && *nptr <= '9') {
        value = value * 10 + (*nptr - '0');
        nptr++;
    }
    return sign * value;
}
