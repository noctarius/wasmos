/* test_wasmos_app_allow.c - the kernel's allowlist of package hashes
 * (wasmos_app_allow_register, src/kernel/wasmos_app.c): "start anyway" for a
 * signed package that no trusted key verifies.
 *
 * Regression: 2026-11-28-start-anyway -- a package signed by a publisher this
 * system does not know could never be started: a signature no trusted key
 * verifies refused the spawn, and nothing let an administrator accept that
 * package by its contents, as macOS and Windows let a user open an app from an
 * unidentified developer.
 *
 * A separate suite because the allowlist is global kernel state. Packages are
 * signed with an Ed25519 key the kernel does not trust; the expected trust level
 * of an allowed one is WASMOS_APP_TRUST_ALLOWED: admitted, but no more trusted
 * than an unsigned package. */

#include "wasmos_app.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/sha512.h"
#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "test_shuffle.h"

typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t flags;
    uint32_t name_len;
    uint32_t entry_len;
    uint32_t wasm_size;
    uint32_t dep_count;
    uint32_t cap_count;
    uint32_t mem_hint_count;
    uint32_t compiled_count;
    char subsystem_tag[8];
    uint32_t region_count;
    uint32_t signature_size;
} test_header_t;

static const uint8_t k_spki_prefix[12] = {
    0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00};

static uint8_t g_blob[1024];

static uint32_t put(uint32_t off, const void* bytes, uint32_t len) {
    memcpy(&g_blob[off], bytes, len);
    return off + len;
}

/* An app package: name "t", entry "m", the 4-byte payload `payload`, declaring
 * an Ed25519 signature record when `signed_pkg`. Returns the length before any
 * record. */
static uint32_t build(const char payload[4], int signed_pkg) {
    test_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, "WASMOSAP", 8);
    hdr.version = WASMOS_APP_VERSION;
    hdr.header_size = (uint16_t)sizeof(hdr);
    hdr.flags = WASMOS_APP_FLAG_APP;
    hdr.name_len = 1;
    hdr.entry_len = 1;
    hdr.wasm_size = 4;
    hdr.mem_hint_count = 2;
    hdr.signature_size = signed_pkg ? WASMOS_APP_SIG_HEAD_LEN + 64u : 0u;
    memcpy(hdr.subsystem_tag, "WASM", 4);
    uint32_t off = put(0, &hdr, sizeof(hdr));
    off = put(off, "t", 1);
    off = put(off, "m", 1);
    uint32_t hints[6] = {WASMOS_APP_MEM_HINT_STACK, 3u, 0u, WASMOS_APP_MEM_HINT_HEAP, 5u, 0u};
    off = put(off, hints, sizeof(hints));
    return put(off, payload, 4);
}

/* Sign the `len` bytes build() left with the Ed25519 key from `seed_byte`
 * repeated, which this kernel does not trust; returns the package length. */
static uint32_t sign(uint32_t len, uint8_t seed_byte) {
    uint8_t seed[32];
    uint8_t secret[64];
    uint8_t pub[32];
    uint8_t der[44];
    uint8_t hash[32];
    memset(seed, seed_byte, sizeof(seed));
    crypto_ed25519_key_pair(secret, pub, seed);
    memcpy(der, k_spki_prefix, sizeof(k_spki_prefix));
    memcpy(der + sizeof(k_spki_prefix), pub, sizeof(pub));
    crypto_blake2b(hash, sizeof(hash), der, sizeof(der));
    uint16_t head[2] = {WASMOS_APP_SIG_ALG_ED25519, 64u};
    memcpy(&g_blob[len], head, sizeof(head));
    memcpy(&g_blob[len + 4u], hash, WASMOS_APP_SIG_KEY_ID_LEN);
    crypto_ed25519_sign(&g_blob[len + WASMOS_APP_SIG_HEAD_LEN], secret, g_blob, len);
    return len + WASMOS_APP_SIG_HEAD_LEN + 64u;
}

static void package_hash(uint32_t len, uint8_t out[64]) {
    (void)mbedtls_sha512(g_blob, len, out, 0);
}

static int test_an_allowed_hash_admits_a_package_no_key_verifies(void) {
    wasmos_app_desc_t desc;
    uint8_t hash[64];
    uint32_t len = sign(build("\0asm", 1), 0x31u);
    if (wasmos_app_parse(g_blob, len, &desc) != 0 ||
        wasmos_app_trust(&desc) != WASMOS_APP_TRUST_INVALID || wasmos_app_admit(&desc) == 0) {
        return __LINE__; /* refused until allowed */
    }
    package_hash(len, hash);
    if (wasmos_app_allow_register(hash, sizeof(hash)) != WASMOS_OK) {
        return __LINE__;
    }
    if (wasmos_app_trust(&desc) != WASMOS_APP_TRUST_ALLOWED || wasmos_app_admit(&desc) != 0 ||
        wasmos_app_signature_valid(&desc) != 0) {
        return __LINE__;
    }
    /* Registering the same digest again changes nothing. */
    if (wasmos_app_allow_register(hash, sizeof(hash)) != WASMOS_OK ||
        wasmos_app_trust(&desc) != WASMOS_APP_TRUST_ALLOWED) {
        return __LINE__;
    }
    /* The digest names these bytes and no others: the same package altered is
     * refused again. */
    g_blob[len - WASMOS_APP_SIG_HEAD_LEN - 64u - 1u] ^= 0x01u;
    if (wasmos_app_parse(g_blob, len, &desc) != 0 ||
        wasmos_app_trust(&desc) != WASMOS_APP_TRUST_INVALID || wasmos_app_admit(&desc) == 0) {
        return __LINE__;
    }
    return 0;
}

static int test_an_unsigned_package_is_not_made_allowed(void) {
    wasmos_app_desc_t desc;
    uint8_t hash[64];
    uint32_t len = build("\0asn", 0);
    package_hash(len, hash);
    if (wasmos_app_allow_register(hash, sizeof(hash)) != WASMOS_OK ||
        wasmos_app_parse(g_blob, len, &desc) != 0 ||
        wasmos_app_trust(&desc) != WASMOS_APP_TRUST_UNSIGNED) {
        return __LINE__;
    }
    return 0;
}

static int test_a_digest_of_the_wrong_length_is_refused(void) {
    uint8_t hash[64];
    memset(hash, 0x11, sizeof(hash));
    if (wasmos_app_allow_register(hash, 63u) != WASMOS_ERR_KERNEL_BAD_HASH ||
        wasmos_app_allow_register(hash, 65u) != WASMOS_ERR_KERNEL_BAD_HASH ||
        wasmos_app_allow_register(0, 64u) != WASMOS_ERR_KERNEL_BAD_HASH) {
        return __LINE__;
    }
    return 0;
}

static int test_only_the_keyring_registers_a_digest(void) {
    extern uint32_t stubs_app_keyring_context;
    wasmos_app_desc_t desc;
    uint8_t hash[64];
    uint32_t len = sign(build("\0aso", 1), 0x32u);
    package_hash(len, hash);
    stubs_app_keyring_context = 9u;
    if (wasmos_app_allow_register_as(8u, hash, sizeof(hash)) != WASMOS_ERR_KERNEL_NOT_AUTHORIZED ||
        wasmos_app_parse(g_blob, len, &desc) != 0 ||
        wasmos_app_trust(&desc) != WASMOS_APP_TRUST_INVALID) {
        return __LINE__;
    }
    if (wasmos_app_allow_register_as(9u, hash, sizeof(hash)) != WASMOS_OK ||
        wasmos_app_trust(&desc) != WASMOS_APP_TRUST_ALLOWED) {
        return __LINE__;
    }
    return 0;
}

static int test_the_allowlist_is_bounded(void) {
    uint8_t hash[64];
    int32_t rc = WASMOS_OK;
    /* Distinct digests until the table refuses one; the cases above may have
     * registered a few already. */
    for (uint32_t i = 0; i <= WASMOS_APP_MAX_ALLOWED && rc == WASMOS_OK; ++i) {
        memset(hash, 0, sizeof(hash));
        memcpy(hash, &i, sizeof(i));
        hash[63] = 0xA5u;
        rc = wasmos_app_allow_register(hash, sizeof(hash));
    }
    return rc == WASMOS_ERR_KERNEL_ALLOWLIST_FULL ? 0 : __LINE__;
}

int main(void) {
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_an_allowed_hash_admits_a_package_no_key_verifies),
        WASMOS_TEST_CASE(test_an_unsigned_package_is_not_made_allowed),
        WASMOS_TEST_CASE(test_a_digest_of_the_wrong_length_is_refused),
        WASMOS_TEST_CASE(test_only_the_keyring_registers_a_digest),
    };
    const int groups = (int)(sizeof(cases) / sizeof(cases[0]));
    if (wasmos_test_run_all(cases, groups) != 0) {
        return 1;
    }
    /* Fills the table, so it runs after the shuffled cases that need room. */
    int line = test_the_allowlist_is_bounded();
    if (line != 0) {
        printf("test_the_allowlist_is_bounded failed at line %d\n", line);
        return 1;
    }
    printf("test_wasmos_app_allow: %d groups passed\n", groups + 1);
    return 0;
}
