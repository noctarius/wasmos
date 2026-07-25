/* mbedtls_config.h - freestanding mbedTLS 3.6 config for the net-stack service.
 *
 * Milestone B scope: a TLS 1.2 *client* that performs an encrypted handshake and
 * GET with NO certificate verification (authmode is forced to
 * MBEDTLS_SSL_VERIFY_NONE by ALTCP_MBEDTLS_AUTHMODE in lwipopts.h). Certificate
 * chain / hostname verification is milestone C.
 *
 * This runs inside the native (ring-0, NO_SYS) net-stack reactor which has:
 *   - no filesystem, no sockets, no host time, no threads,
 *   - no malloc (mbedTLS uses its own static buffer allocator, see below),
 *   - a pre-seeded hardware entropy pool drained by mbedtls_hardware_poll()
 *     (net_stack.c), fed by the `hrng` virtual service at startup.
 *
 * Only the modules needed for ECDHE-RSA / ECDHE-ECDSA + AES-GCM + SHA-2 are
 * enabled; everything that would drag in libc stdio/fs/time is left off.
 */
#ifndef WASMOS_NET_STACK_MBEDTLS_CONFIG_H
#define WASMOS_NET_STACK_MBEDTLS_CONFIG_H

/* --- Platform / freestanding ------------------------------------------------
 * PLATFORM_C + PLATFORM_MEMORY let the buffer allocator install itself as
 * mbedtls_calloc/free at runtime (mbedtls_memory_buffer_alloc_init). The
 * pre-init calloc default resolves to mbedTLS's internal platform_calloc_uninit
 * (returns NULL, never actually called), so no stdlib calloc is linked. */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_MEMORY_BUFFER_ALLOC_C
/* No stdlib in this freestanding service. Route mbedtls_exit (only ever reached
 * from buffer-allocator verify paths, which are disabled) to a trap so nothing
 * pulls in libc exit(). calloc/free are installed at runtime by the buffer
 * allocator; their pre-init default resolves to mbedTLS's internal
 * platform_calloc_uninit, so no stdlib allocator is linked either. */
#define MBEDTLS_PLATFORM_EXIT_MACRO(status) __builtin_trap()

/* No host services: MBEDTLS_FS_IO, MBEDTLS_NET_C, MBEDTLS_TIMING_C,
 * MBEDTLS_HAVE_TIME, MBEDTLS_HAVE_TIME_DATE, MBEDTLS_SELF_TEST,
 * MBEDTLS_THREADING_C, MBEDTLS_PSA_CRYPTO_C, MBEDTLS_SSL_PROTO_TLS1_3 are all
 * deliberately left undefined. */

/* --- Entropy / RNG ---------------------------------------------------------- */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C
/* The only entropy source is mbedtls_hardware_poll() (implemented in
 * net_stack.c, draining the hrng-filled pool). No platform entropy. */
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY

/* Freestanding platform std functions. The link is -nostdlib with no
 * compiler-rt and no libc stdio/stdlib, so:
 *   - MBEDTLS_HAVE_INT32 forces 32-bit bignum limbs, avoiding the __int128
 *     division (__udivti3) that 64-bit limbs would emit.
 *   - calloc/free are only the pre-init values of the buffer allocator's
 *     function pointers (replaced at runtime by mbedtls_memory_buffer_alloc_init)
 *     and are never actually called; snprintf is only reached from X.509/debug
 *     string helpers the no-verify client path does not use. Route all three to
 *     net-stack stubs (net_stack.c) so nothing pulls in libc. */
#define MBEDTLS_HAVE_INT32
#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdarg.h>
void* net_stack_mbedtls_std_calloc(size_t nmemb, size_t size);
void net_stack_mbedtls_std_free(void* ptr);
int net_stack_mbedtls_snprintf(char* buf, size_t size, const char* fmt, ...);
#endif
#define MBEDTLS_PLATFORM_STD_CALLOC net_stack_mbedtls_std_calloc
#define MBEDTLS_PLATFORM_STD_FREE net_stack_mbedtls_std_free
#define MBEDTLS_PLATFORM_SNPRINTF_MACRO net_stack_mbedtls_snprintf

/* --- Big number / public key ------------------------------------------------ */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C

#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21

/* Elliptic curves: ECDHE key exchange + ECDSA cert/signature verification. */
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED

/* --- Symmetric / hashing ---------------------------------------------------- */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

/* --- X.509 (parse the server certificate chain; no verification) ------------ */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* --- TLS 1.2 client --------------------------------------------------------- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2

/* Key exchanges for a modern TLS 1.2 server (forward-secret ECDHE). */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

/* Note: mbedtls/build_info.h runs mbedtls/check_config.h itself, AFTER the
 * config_adjust_*.h headers derive MBEDTLS_MD_CAN_* / MBEDTLS_CAN_ECDH / etc.
 * Do NOT #include "mbedtls/check_config.h" here — doing so runs the checks
 * before those derived capability macros exist and fails spuriously. */

#endif /* WASMOS_NET_STACK_MBEDTLS_CONFIG_H */
