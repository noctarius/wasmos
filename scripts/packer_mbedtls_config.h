/* packer_mbedtls_config.h - the mbedTLS subset make_wasmos_app links on the
 * host: reading an RSA private key from PEM, signing a package with RSASSA-PSS
 * over SHA-512, writing a public key as DER or PEM, and generating a test key
 * (make_wasmos_app_rsa.c). The kernel verifies with its own subset
 * (src/kernel/crypto/kernel_mbedtls_config.h). Host libc provides memory and
 * stdio; randomness comes from /dev/urandom, passed in by the caller. */
#ifndef WASMOS_PACKER_MBEDTLS_CONFIG_H
#define WASMOS_PACKER_MBEDTLS_CONFIG_H

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_FS_IO
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_GENPRIME
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PEM_WRITE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_MD_C
#define MBEDTLS_SHA512_C

#endif /* WASMOS_PACKER_MBEDTLS_CONFIG_H */
