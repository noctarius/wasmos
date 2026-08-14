/* mbedtls_config.h - freestanding mbedTLS 3.6 config for the net-stack service.
 *
 * Scope: a TLS 1.2 *client* that verifies the server certificate chain against a
 * FS-loaded CA trust store and checks the hostname (authmode is
 * MBEDTLS_SSL_VERIFY_REQUIRED via ALTCP_MBEDTLS_AUTHMODE in lwipopts.h;
 * net_stack.c calls mbedtls_ssl_set_hostname per connection). X.509 chain + name
 * verification below is fully enabled.
 * TODO: certificate date validity is NOT checked — MBEDTLS_HAVE_TIME /
 * MBEDTLS_HAVE_TIME_DATE stay off because no RTC is wired to mbedTLS, so
 * notBefore/notAfter are skipped and an expired certificate is accepted.
 *
 * This runs inside the native (ring-0, NO_SYS) net-stack reactor which has:
 *   - no filesystem, no sockets, no host time, no threads,
 *   - mbedtls_calloc/free routed to the native slab allocator (heap_native.c),
 *     so the TLS heap grows on demand from kernel pages,
 *   - a pre-seeded hardware entropy pool drained by mbedtls_hardware_poll()
 *     (net_stack.c), fed by the `hrng` virtual service at startup.
 *
 * Only the modules needed for ECDHE-RSA / ECDHE-ECDSA + AES-GCM + SHA-2 are
 * enabled; everything that would drag in libc stdio/fs/time is left off.
 */
#ifndef WASMOS_NET_STACK_MBEDTLS_CONFIG_H
#define WASMOS_NET_STACK_MBEDTLS_CONFIG_H

/* --- Platform / freestanding ------------------------------------------------
 * PLATFORM_C + PLATFORM_MEMORY make mbedtls_calloc/free resolve to the
 * MBEDTLS_PLATFORM_CALLOC_MACRO/FREE_MACRO wrappers below, which forward to the
 * native slab allocator (heap_native.c) — so the TLS heap grows on demand from
 * kernel pages (no fixed static pool). Route mbedtls_exit to a trap so nothing
 * pulls in a libc exit(); it is only reachable from disabled self-test/verify
 * paths. */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
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

/* Freestanding platform functions. The link is -nostdlib with no compiler-rt
 * and no libc stdio/stdlib, so:
 *   - MBEDTLS_HAVE_INT32 forces 32-bit bignum limbs, avoiding the __int128
 *     division (__udivti3) that 64-bit limbs would emit.
 *   - mbedtls_calloc/free bind at COMPILE TIME (the *_MACRO forms, not the
 *     STD_* runtime pointers) to net-stack wrappers over the native slab
 *     allocator (heap_native.c), so the TLS heap grows on demand from kernel
 *     pages. Using the macro forms is load-bearing: lwIP's altcp_tls layer
 *     (altcp_tls_mbedtls_mem.c) otherwise calls mbedtls_platform_set_calloc_free
 *     at every config-create to redirect ALL mbedTLS allocation into lwIP's
 *     fixed MEM_SIZE (64 KiB) heap — far too small for a full CA bundle (the
 *     chain parse fails with X509_ALLOC_FAILED after ~20 certs). Defining both
 *     *_MACRO forms compiles out that override (its guard
 *     ALTCP_MBEDTLS_PLATFORM_ALLOC evaluates to 0) AND makes
 *     mbedtls_platform_set_calloc_free unavailable, so the growable native heap
 *     stands and TLS runtime buffers stay out of the cramped lwIP heap.
 *   - snprintf is only reached from X.509/debug string helpers; route it to a
 *     net-stack stub so nothing pulls in libc. */
#define MBEDTLS_HAVE_INT32
#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdarg.h>
void* net_stack_mbedtls_std_calloc(size_t nmemb, size_t size);
void net_stack_mbedtls_std_free(void* ptr);
int net_stack_mbedtls_snprintf(char* buf, size_t size, const char* fmt, ...);
#endif
#define MBEDTLS_PLATFORM_CALLOC_MACRO net_stack_mbedtls_std_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO net_stack_mbedtls_std_free
#define MBEDTLS_PLATFORM_SNPRINTF_MACRO net_stack_mbedtls_snprintf

/* --- Big number / public key ------------------------------------------------
 * The floor under certificate parsing and signature verification: DER/ASN.1
 * decoding plus the OID table that names algorithms and X.509 attributes, and
 * the generic PK layer the X.509 code verifies through.  ASN1_WRITE is pulled in
 * by the PK/X.509 writing helpers the library links unconditionally.
 * RSA with both PKCS#1 padding schemes stays enabled because a server
 * certificate chain still commonly signs with RSA (v1.5 for older signatures,
 * v2.1/PSS for newer) even when the key exchange is ECDHE. */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C

#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21

/* Elliptic curves: ECDHE key exchange + ECDSA cert/signature verification.
 * The three curves are what a public TLS 1.2 server realistically offers;
 * every other curve mbedTLS supports is left out because each one costs code
 * size and a table in a service that only ever acts as a client. */
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED

/* --- Symmetric / hashing ----------------------------------------------------
 * Exactly what the two enabled key exchanges need and nothing else: AES-GCM as
 * the only record cipher (so no CBC, no ChaCha20, no stream ciphers), reached
 * through the generic CIPHER layer the TLS record code uses, and the SHA-2
 * family for the handshake transcript, the PRF, and certificate signatures.
 * SHA224/SHA384 come with their 256/512 cores at negligible cost and cover
 * certificates signed with them. */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

/* --- X.509 (parse AND verify the server certificate chain + hostname) ------- */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
/* The CA trust store is a PEM bundle (concatenated base64 certificates) loaded
 * from the filesystem, so PEM decoding must be enabled; the certificates that
 * arrive over the wire are DER and would not need it.  MBEDTLS_PEM_PARSE_C pulls
 * in base64 decoding. */
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C

/* --- TLS 1.2 client ---------------------------------------------------------
 * Client role only: MBEDTLS_SSL_SRV_C is absent, so the server half of the
 * handshake is not compiled in at all (lwIP's altcp_tls server config path
 * therefore cannot be used, which is why the mbedTLS 3.x server-side shims in
 * net_stack_mbedtls3_compat.h only have to compile, never run).  TLS 1.3 is off
 * as well — it would require MBEDTLS_PSA_CRYPTO_C, which this freestanding
 * build does not carry. */
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
