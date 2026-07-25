/* Compatibility shim: mbedtls/certs.h was removed in mbedTLS 3.x.
 *
 * lwIP 2.2.1's altcp_tls_mbedtls.c (a never-edited git subtree) still includes
 * this 2.x header unconditionally, but only uses the built-in test certificates
 * for server test paths that the net-stack never exercises. Providing an empty
 * header on an include path ahead of libs/mbedtls/include satisfies the include
 * without touching libs/. */
#ifndef WASMOS_NET_STACK_MBEDTLS_CERTS_SHIM_H
#define WASMOS_NET_STACK_MBEDTLS_CERTS_SHIM_H
#endif
