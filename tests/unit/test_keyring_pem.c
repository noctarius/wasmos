/* test_keyring_pem.c - the keyring's pure helpers (src/services/keyring/
 * keyring_pem.c): reading a PEM public key, recognising its algorithm,
 * deriving its key id as the kernel and the packer do, and picking key files
 * out of a directory listing. Regression: 2026-11-28-keyring (the service that
 * makes publisher keys trusted). */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keyring_pem.h"
#include "test_shuffle.h"

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                          \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

/* make_wasmos_app --pubkey-pem of the test seed 00 01 ... 1f, the system key of
 * tests/unit/stubs_signing_pubkey.c, whose id the kernel stub also carries. */
static const char k_ed25519_pem[] = "-----BEGIN PUBLIC KEY-----\n"
                                    "MCowBQYDK2VwAyEAA6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=\n"
                                    "-----END PUBLIC KEY-----\n";
static const uint8_t k_ed25519_id[KEYRING_KEY_ID_LEN] = {
    0x40, 0x2d, 0x8a, 0xb0, 0x0f, 0xef, 0xe6, 0xc9, 0x01, 0x52, 0x29, 0x75, 0xbd, 0xd2, 0xfd, 0xda};

/* A 2048-bit RSA public key (openssl rsa -pubout), a throwaway test key. */
static const char k_rsa_pem[] = "-----BEGIN PUBLIC KEY-----\n"
                                "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAlY0mtYdWemasPCus4Hne\n"
                                "k27aWJhJ+oWy+2pemahbkr++9JYuPuzc6A5KV8fwnGZcHjcca13ZJpjyuyA7aTvW\n"
                                "6ms3q0vY9/p50ZW/VNdh4DSdIvON0DUDc29t3obWfQdNM/fhAvJxHqe9z7vu4w40\n"
                                "jwwWxseLJJmaNrt0wpu7tMaUc6ouekBRrBSji2RIHw5EonB+/rEYXl9AaQpRTlEV\n"
                                "WR0Lr15Mnw3dQ7H8IpeqNhvmrXfV/Kti+LKG20QbVhz3cgIZSccbJkJP8TjoksTw\n"
                                "N7YrFk99i6sunboaarmn+eKzPiSXLv7t96llNyms3hHIUSidGRfeQWDeRLHFfXuN\n"
                                "bwIDAQAB\n"
                                "-----END PUBLIC KEY-----\n";

static void test_an_ed25519_key_reads_as_the_packer_wrote_it(void) {
    uint8_t der[128];
    int32_t len =
        keyring_pem_to_der(k_ed25519_pem, (uint32_t)strlen(k_ed25519_pem), der, sizeof(der));
    CHECK(len == 44, "an Ed25519 SubjectPublicKeyInfo is 44 bytes");
    CHECK(keyring_der_kind(der, (uint32_t)len) == KEYRING_KEY_ED25519, "recognised as Ed25519");
    uint8_t id[KEYRING_KEY_ID_LEN];
    keyring_key_id(der, (uint32_t)len, id);
    CHECK(memcmp(id, k_ed25519_id, sizeof(id)) == 0, "the key id is the kernel's and the packer's");
}

static void test_an_rsa_key_is_recognised(void) {
    uint8_t der[1024];
    int32_t len = keyring_pem_to_der(k_rsa_pem, (uint32_t)strlen(k_rsa_pem), der, sizeof(der));
    CHECK(len == 294, "a 2048-bit RSA SubjectPublicKeyInfo is 294 bytes");
    CHECK(keyring_der_kind(der, (uint32_t)len) == KEYRING_KEY_RSA, "recognised as RSA");
}

static void test_malformed_pem_is_refused(void) {
    uint8_t der[128];
    static const char no_end[] = "-----BEGIN PUBLIC KEY-----\nMCowBQYDK2VwAyEA\n";
    static const char bad_char[] =
        "-----BEGIN PUBLIC KEY-----\nMCow*QYDK2VwAyEA\n-----END PUBLIC KEY-----\n";
    static const char after_pad[] =
        "-----BEGIN PUBLIC KEY-----\nMCo=wBQY\n-----END PUBLIC KEY-----\n";
    CHECK(keyring_pem_to_der(no_end, sizeof(no_end) - 1u, der, sizeof(der)) == -1, "no end line");
    CHECK(keyring_pem_to_der(bad_char, sizeof(bad_char) - 1u, der, sizeof(der)) == -1,
          "a character outside base64");
    CHECK(keyring_pem_to_der(after_pad, sizeof(after_pad) - 1u, der, sizeof(der)) == -1,
          "data after padding");
    CHECK(keyring_pem_to_der(k_ed25519_pem, (uint32_t)strlen(k_ed25519_pem), der, 43u) == -1,
          "a key larger than the buffer");
    uint8_t bogus[44] = {0x30, 0x2a};
    CHECK(keyring_der_kind(bogus, sizeof(bogus)) == KEYRING_KEY_UNKNOWN, "an unknown key");
}

static void test_only_pem_files_are_picked_from_a_listing(void) {
    static const char listing[] = "system.pem\nsub/\nREADME\napps.pem\r\nx.pem/\n.pem\nlast.pem";
    char name[16];
    uint32_t pos = 0;
    CHECK(keyring_listing_next(listing, sizeof(listing) - 1u, &pos, name, sizeof(name)) == 1 &&
              strcmp(name, "system.pem") == 0,
          "the first key file");
    CHECK(keyring_listing_next(listing, sizeof(listing) - 1u, &pos, name, sizeof(name)) == 1 &&
              strcmp(name, "apps.pem") == 0,
          "a directory and another file are skipped, a CR is trimmed");
    CHECK(keyring_listing_next(listing, sizeof(listing) - 1u, &pos, name, sizeof(name)) == 1 &&
              strcmp(name, "last.pem") == 0,
          "a directory named .pem and a bare .pem are skipped; no final newline needed");
    CHECK(keyring_listing_next(listing, sizeof(listing) - 1u, &pos, name, sizeof(name)) == 0,
          "the end");
    pos = 0;
    CHECK(keyring_listing_next(listing, sizeof(listing) - 1u, &pos, name, 6u) == 0,
          "a name that does not fit is skipped");
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_an_ed25519_key_reads_as_the_packer_wrote_it),
        WASMOS_TEST_CASE(test_an_rsa_key_is_recognised),
        WASMOS_TEST_CASE(test_malformed_pem_is_refused),
        WASMOS_TEST_CASE(test_only_pem_files_are_picked_from_a_listing),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_keyring_pem: ok\n");
    return 0;
}
