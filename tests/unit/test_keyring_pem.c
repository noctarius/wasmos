/* test_keyring_pem.c - the keyring's pure helpers (src/services/keyring/
 * keyring_pem.c): reading a PEM public key, recognising its algorithm,
 * deriving its key id as the kernel and the packer do, picking key files out
 * of a directory listing, and reading and writing the package allowlist.
 * Regression: 2026-11-28-keyring (the service that makes publisher keys
 * trusted); 2026-11-28-start-anyway (the allowlist). */

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

/* A line written for a digest reads back as that digest; the reader skips what
 * is not a digest line and says so for a malformed one. */
static void test_an_allowlist_line_reads_back_as_its_digest(void) {
    uint8_t digest[KEYRING_DIGEST_LEN];
    for (uint32_t i = 0; i < KEYRING_DIGEST_LEN; ++i) {
        digest[i] = (uint8_t)(i * 7u + 3u);
    }
    char text[4 * KEYRING_ALLOW_LINE_MAX];
    uint32_t len = 0;
    static const char head[] = "# packages allowed to start anyway\n\n";
    memcpy(text, head, sizeof(head) - 1u);
    len += (uint32_t)sizeof(head) - 1u;
    static const char path[] = "/boot/apps/a\nb.wap";
    int32_t n = keyring_allow_line(
        digest, path, (uint32_t)sizeof(path) - 1u, text + len, (uint32_t)sizeof(text) - len);
    CHECK(n == (int32_t)(2u * KEYRING_DIGEST_LEN + 1u + sizeof(path) - 1u + 1u), "line length");
    CHECK(text[len + n - 1] == '\n', "a final newline");
    CHECK(memchr(text + len, '\n', (size_t)n - 1u) == NULL, "a newline in the path is not kept");
    len += (uint32_t)n;
    static const char bad[] = "not a digest\n";
    memcpy(text + len, bad, sizeof(bad) - 1u);
    len += (uint32_t)sizeof(bad) - 1u;
    /* A bare digest, upper case, without a final newline. */
    for (uint32_t i = 0; i < KEYRING_DIGEST_LEN; ++i) {
        static const char hex[] = "0123456789ABCDEF";
        text[len++] = hex[(uint8_t)(255u - i) >> 4];
        text[len++] = hex[(uint8_t)(255u - i) & 15u];
    }

    uint8_t got[KEYRING_DIGEST_LEN];
    uint32_t pos = 0;
    CHECK(keyring_allow_next(text, len, &pos, got) == 1 && memcmp(got, digest, sizeof(got)) == 0,
          "the written line reads back");
    CHECK(keyring_allow_next(text, len, &pos, got) == -1, "a malformed line is reported");
    CHECK(keyring_allow_next(text, len, &pos, got) == 1 && got[0] == 255u && got[63] == 192u,
          "a bare upper-case digest at the end");
    CHECK(keyring_allow_next(text, len, &pos, got) == 0, "the end");
}

static void test_an_allowlist_line_must_fit(void) {
    uint8_t digest[KEYRING_DIGEST_LEN] = {0};
    char path[KEYRING_ALLOW_PATH_MAX + 1u];
    char out[KEYRING_ALLOW_LINE_MAX];
    memset(path, 'p', sizeof(path));
    CHECK(keyring_allow_line(digest, path, KEYRING_ALLOW_PATH_MAX, out, sizeof(out)) ==
              (int32_t)sizeof(out),
          "the longest path fits the longest line");
    CHECK(keyring_allow_line(digest, path, sizeof(path), out, sizeof(out)) == -1,
          "a longer path is refused");
    CHECK(keyring_allow_line(digest, path, 4u, out, 2u * KEYRING_DIGEST_LEN) == -1,
          "a line longer than the buffer is refused");
}

static void test_a_key_file_is_named_by_its_id(void) {
    const uint8_t id[KEYRING_KEY_ID_LEN] = {0xde, 0xad, 0x0b, 0xef, 1, 2, 3};
    char name[13];
    keyring_key_file_name(id, name);
    CHECK(strcmp(name, "dead0bef.pem") == 0, "8 hex digits and .pem");
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_an_ed25519_key_reads_as_the_packer_wrote_it),
        WASMOS_TEST_CASE(test_an_rsa_key_is_recognised),
        WASMOS_TEST_CASE(test_malformed_pem_is_refused),
        WASMOS_TEST_CASE(test_only_pem_files_are_picked_from_a_listing),
        WASMOS_TEST_CASE(test_an_allowlist_line_reads_back_as_its_digest),
        WASMOS_TEST_CASE(test_an_allowlist_line_must_fit),
        WASMOS_TEST_CASE(test_a_key_file_is_named_by_its_id),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_keyring_pem: ok\n");
    return 0;
}
