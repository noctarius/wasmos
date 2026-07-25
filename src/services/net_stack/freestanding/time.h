/* Minimal freestanding <time.h> for the net-stack altcp_tls glue TUs.
 *
 * The glue is compiled with MBEDTLS_HAVE_TIME so mbedtls_ssl_session gains its
 * `start` member (which lwIP 2.2.1's altcp_tls_set_session references). Under
 * HAVE_TIME mbedTLS's platform.h/platform_time.h include <time.h> for time_t.
 * The glue never actually calls time()/clock_gettime() (nor does net-stack use
 * TLS session resumption), so declarations suffice; the kernel has no <time.h>.
 * On this include path for the altcp_tls glue TUs only. */
#ifndef WASMOS_NET_STACK_TIME_SHIM_H
#define WASMOS_NET_STACK_TIME_SHIM_H

#include <stddef.h>

typedef long time_t;

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif

time_t time(time_t *t);
int clock_gettime(int clk_id, struct timespec *tp);

#endif /* WASMOS_NET_STACK_TIME_SHIM_H */
