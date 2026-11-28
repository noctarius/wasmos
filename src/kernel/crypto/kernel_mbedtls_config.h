/* kernel_mbedtls_config.h - the mbedTLS subset the kernel links: verifying an
 * RSA-PSS-SHA512 package signature against a registered RSA public key
 * (wasmos_app_trust, src/kernel/wasmos_app.c). Nothing else: no TLS, no X.509,
 * no private-key operations, no randomness, no time, no files.
 *
 * Freestanding as net-stack's configuration is (net_stack_mbedtls_config.h):
 * 32-bit bignum limbs so no __int128 division (__udivti3) is emitted, and
 * mbedtls_calloc/free bound at compile time to kernel wrappers
 * (kernel_mbedtls.c) over the kernel allocator. mbedtls/build_info.h runs
 * check_config.h after this file. */
#ifndef WASMOS_KERNEL_MBEDTLS_CONFIG_H
#define WASMOS_KERNEL_MBEDTLS_CONFIG_H

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_EXIT_MACRO(status) __builtin_trap()
#define MBEDTLS_HAVE_INT32

#ifndef __ASSEMBLER__
#include <stddef.h>
void* kernel_mbedtls_calloc(size_t nmemb, size_t size);
void kernel_mbedtls_free(void* ptr);
int kernel_mbedtls_snprintf(char* buf, size_t size, const char* fmt, ...);
#endif
#define MBEDTLS_PLATFORM_CALLOC_MACRO kernel_mbedtls_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO kernel_mbedtls_free
#define MBEDTLS_PLATFORM_SNPRINTF_MACRO kernel_mbedtls_snprintf

/* Parsing a DER SubjectPublicKeyInfo and verifying RSASSA-PSS over a SHA-512
 * digest. */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_MD_C
#define MBEDTLS_SHA512_C

#endif /* WASMOS_KERNEL_MBEDTLS_CONFIG_H */
