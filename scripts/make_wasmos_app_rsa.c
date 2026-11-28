/* make_wasmos_app_rsa.c - see make_wasmos_app_rsa.h. */
#include "make_wasmos_app_rsa.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/sha512.h"

#define RSA_HEAD_LEN 20u
#define RSA_ALG_PSS_SHA512 2u

/* Host randomness for the PSS salt, RSA blinding and key generation. */
static int urandom(void* ctx, unsigned char* out, size_t len) {
    (void)ctx;
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) {
        return -1;
    }
    size_t got = fread(out, 1, len, f);
    fclose(f);
    return got == len ? 0 : -1;
}

static int load_key(mbedtls_pk_context* pk, const char* key_path) {
    mbedtls_pk_init(pk);
    int rc = mbedtls_pk_parse_keyfile(pk, key_path, NULL, urandom, NULL);
    if (rc != 0 || mbedtls_pk_get_type(pk) != MBEDTLS_PK_RSA) {
        fprintf(stderr, "%s: not an RSA private key (-0x%04x)\n", key_path, (unsigned)-rc);
        mbedtls_pk_free(pk);
        return -1;
    }
    size_t bits = mbedtls_pk_get_bitlen(pk);
    if (bits < 2048u || bits > 4096u) {
        fprintf(
            stderr, "%s: a %u-bit key; 2048 to 4096 bits are accepted\n", key_path, (unsigned)bits);
        mbedtls_pk_free(pk);
        return -1;
    }
    return 0;
}

int rsa_signature_len(const char* key_path) {
    mbedtls_pk_context pk;
    if (load_key(&pk, key_path) != 0) {
        return -1;
    }
    int len = (int)mbedtls_pk_get_len(&pk);
    mbedtls_pk_free(&pk);
    return len;
}

int rsa_sign_package(const char* path, const char* key_path,
                     void (*key_id)(const uint8_t* der, uint32_t len, uint8_t id[16])) {
    mbedtls_pk_context pk;
    if (load_key(&pk, key_path) != 0) {
        return -1;
    }
    int rc = -1;
    uint8_t* data = NULL;
    uint8_t* record = NULL;
    FILE* f = fopen(path, "rb");
    long size = -1;
    if (f && fseek(f, 0, SEEK_END) == 0) {
        size = ftell(f);
    }
    if (size > 0 && fseek(f, 0, SEEK_SET) == 0) {
        data = (uint8_t*)malloc((size_t)size);
    }
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "%s: cannot read back for signing\n", path);
        goto out;
    }
    fclose(f);
    f = NULL;

    size_t sig_len = mbedtls_pk_get_len(&pk);
    record = (uint8_t*)calloc(1, RSA_HEAD_LEN + sig_len);
    uint8_t der[1024];
    int der_len = mbedtls_pk_write_pubkey_der(&pk, der, sizeof(der));
    uint8_t digest[64];
    if (!record || der_len <= 0 || mbedtls_sha512(data, (size_t)size, digest, 0) != 0) {
        goto out;
    }
    uint16_t head[2] = {RSA_ALG_PSS_SHA512, (uint16_t)sig_len};
    memcpy(record, head, sizeof(head));
    /* mbedtls_pk_write_pubkey_der writes at the END of the buffer. */
    key_id(der + sizeof(der) - (size_t)der_len, (uint32_t)der_len, record + 4);
    mbedtls_rsa_context* rsa = mbedtls_pk_rsa(pk);
    if (mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA512) != 0 ||
        mbedtls_rsa_rsassa_pss_sign(
            rsa, urandom, NULL, MBEDTLS_MD_SHA512, sizeof(digest), digest, record + RSA_HEAD_LEN) !=
            0) {
        fprintf(stderr, "%s: RSA-PSS signing failed\n", path);
        goto out;
    }
    f = fopen(path, "ab");
    if (!f || fwrite(record, 1, RSA_HEAD_LEN + sig_len, f) != RSA_HEAD_LEN + sig_len) {
        fprintf(stderr, "%s: cannot append the signature\n", path);
        goto out;
    }
    rc = fclose(f) == 0 ? 0 : -1;
    f = NULL;
out:
    if (f) {
        fclose(f);
    }
    free(record);
    free(data);
    mbedtls_pk_free(&pk);
    return rc;
}

int rsa_genkey(const char* out_path, unsigned bits) {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    unsigned char pem[8192];
    int rc = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (rc == 0) {
        rc = mbedtls_rsa_gen_key(mbedtls_pk_rsa(pk), urandom, NULL, bits, 65537);
    }
    if (rc == 0) {
        rc = mbedtls_pk_write_key_pem(&pk, pem, sizeof(pem));
    }
    mbedtls_pk_free(&pk);
    if (rc != 0) {
        fprintf(stderr, "RSA key generation failed (-0x%04x)\n", (unsigned)-rc);
        return -1;
    }
    FILE* f = fopen(out_path, "w");
    if (!f || fputs((const char*)pem, f) < 0) {
        perror(out_path);
        if (f) {
            fclose(f);
        }
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

int rsa_pubkey_pem(const char* key_path, const char* out_path) {
    mbedtls_pk_context pk;
    if (load_key(&pk, key_path) != 0) {
        return -1;
    }
    unsigned char pem[2048];
    int rc = mbedtls_pk_write_pubkey_pem(&pk, pem, sizeof(pem));
    mbedtls_pk_free(&pk);
    if (rc != 0) {
        return -1;
    }
    FILE* f = fopen(out_path, "w");
    if (!f || fputs((const char*)pem, f) < 0) {
        perror(out_path);
        if (f) {
            fclose(f);
        }
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}
