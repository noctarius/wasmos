/* Compatibility shim: mbedtls/ssl_internal.h was removed in mbedTLS 3.x (its
 * contents moved into the private library/ssl_misc.h).
 *
 * lwIP 2.2.1's altcp_tls_mbedtls.c (a never-edited git subtree) includes it
 * solely to call mbedtls_ssl_flush_output() after an ERR_MEM. That function is
 * still a real, externally-linkable symbol in mbedTLS 3.6.7 (defined in
 * ssl_msg.c); it is simply no longer declared in a public header. Re-declaring
 * the prototype here — on an include path ahead of libs/mbedtls/include — lets
 * the glue compile and link without editing libs/. */
#ifndef WASMOS_NET_STACK_MBEDTLS_SSL_INTERNAL_SHIM_H
#define WASMOS_NET_STACK_MBEDTLS_SSL_INTERNAL_SHIM_H

#include "mbedtls/ssl.h"

#ifdef __cplusplus
extern "C" {
#endif

int mbedtls_ssl_flush_output(mbedtls_ssl_context *ssl);

#ifdef __cplusplus
}
#endif

#endif /* WASMOS_NET_STACK_MBEDTLS_SSL_INTERNAL_SHIM_H */
