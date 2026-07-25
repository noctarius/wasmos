/* net_stack_mbedtls3_compat.h - force-included (clang -include) ahead of every
 * other header when compiling the lwIP altcp_tls glue against mbedTLS 3.6.
 *
 * The glue is lwIP 2.2.1 (a never-edited git subtree) written for the mbedTLS
 * 2.x API. Two source-level incompatibilities remain after the header shims
 * (mbedtls/certs.h, mbedtls/ssl_internal.h); both are bridged here without
 * editing libs/:
 *
 *  1. Private struct access. In 3.x, mbedTLS struct members are wrapped in
 *     MBEDTLS_PRIVATE() and only accessible directly when
 *     MBEDTLS_ALLOW_PRIVATE_ACCESS is defined before the struct is seen. The
 *     glue reads `ssl_context.out_left` directly, so define it up front.
 *
 *  2. mbedtls_pk_parse_key() gained two trailing RNG arguments (f_rng, p_rng)
 *     in 3.0. The glue calls it with the 5-argument 2.x signature (only in the
 *     server-config path the net-stack never reaches, but it must still
 *     compile). We pre-include mbedtls/pk.h HERE so its real 7-argument
 *     prototype is processed once, guard-protected, before the macro below
 *     exists; the later `#include "mbedtls/pk.h"` from the glue is a no-op. The
 *     function-like macro then rewrites only the 5-argument call sites, adding
 *     NULL,NULL, and the parenthesized `(mbedtls_pk_parse_key)` reaches the real
 *     function without re-expanding.
 */
#ifndef WASMOS_NET_STACK_MBEDTLS3_COMPAT_H
#define WASMOS_NET_STACK_MBEDTLS3_COMPAT_H

#define MBEDTLS_ALLOW_PRIVATE_ACCESS

/* lwIP 2.2.1's altcp_tls_set_session() references mbedtls_ssl_session.start,
 * which exists only under MBEDTLS_HAVE_TIME. Enable it for the glue TUs ONLY
 * (via this force-included header) so the glue compiles. This is layout-safe:
 * only mbedtls_ssl_session depends on HAVE_TIME, and the glue embeds it solely
 * in altcp_tls_session (used only by the session-resumption helpers net-stack
 * never calls); mbedtls_ssl_config/mbedtls_ssl_context — which the glue embeds
 * by value and passes to the (no-HAVE_TIME) library — do not depend on it. A
 * <time.h> shim satisfies the resulting include; the time functions are never
 * actually called. */
#define MBEDTLS_HAVE_TIME

/* MBEDTLS_SSL_MAX_CONTENT_LEN was split into IN/OUT_CONTENT_LEN in mbedTLS 3.x;
 * the glue still uses the old name in one debug size check. Alias it. */
#define MBEDTLS_SSL_MAX_CONTENT_LEN MBEDTLS_SSL_OUT_CONTENT_LEN

#include "mbedtls/pk.h"

#define mbedtls_pk_parse_key(ctx, key, keylen, pwd, pwdlen) \
    (mbedtls_pk_parse_key)((ctx), (key), (keylen), (pwd), (pwdlen), NULL, NULL)

#endif /* WASMOS_NET_STACK_MBEDTLS3_COMPAT_H */
