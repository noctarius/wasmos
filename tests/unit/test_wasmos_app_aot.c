/* Host unit test for the .wap v12 AOT table in wasmos_app_parse() and its
 * selection, wasmos_app_compiled_for() (src/kernel/wasmos_app.c): a package
 * carries up to WASMOS_APP_MAX_COMPILED WARP AOT binaries, each tagged with the
 * ELF e_machine of its code, and a kernel takes only the one for its own
 * architecture.
 *
 * Regression: 2026-11-28-wap-aot-untagged -- the AOT binary was untagged, so a
 * package built for x86_64 handed its x86_64 code to an aarch64 kernel, which
 * executed it at EL0 and panicked.
 *
 * wasmos_app.c is compiled in for real with the subsystem registry under it;
 * the runtime start hooks are stubbed (stubs_app_runtime.c). Blobs are built
 * byte by byte in the packer's section order. Each case returns 0 to pass or
 * __LINE__ to fail (test_shuffle.h). */

#include "wasmos_app.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "monocypher-ed25519.h"
#include "test_shuffle.h"

#define EM_X86_64 62u
#define EM_AARCH64 183u
#define SIG_RECORD_LEN (WASMOS_APP_SIG_HEAD_LEN + WASMOS_APP_SIG_ED25519_LEN)

/* Mirrors wasmos_app_header_t in wasmos_app.c and scripts/make_wasmos_app.c. */
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

typedef struct {
    uint16_t machine;
    const char* bytes; /* the binary's content; its length is the record's size */
} test_aot_t;

static uint8_t g_blob[1024];

static uint32_t put(uint32_t off, const void* bytes, uint32_t len) {
    memcpy(&g_blob[off], bytes, len);
    return off + len;
}

/* Build an app package: name "t", entry "m", no dependencies or capabilities,
 * a stack and a heap hint, the 4-byte payload "\0asm", then `naot` table
 * records with the binaries after them, then `sig` bytes of signature. The
 * header's compiled_count is `count_field`. Returns the blob length. */
static uint32_t build(uint32_t count_field, const test_aot_t* aot, uint32_t naot, uint32_t sig) {
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
    hdr.compiled_count = count_field;
    hdr.signature_size = sig;
    memcpy(hdr.subsystem_tag, "WASM", 4);

    uint32_t off = put(0, &hdr, sizeof(hdr));
    off = put(off, "t", 1);
    off = put(off, "m", 1);
    uint32_t hints[6] = {WASMOS_APP_MEM_HINT_STACK, 3u, 0u, WASMOS_APP_MEM_HINT_HEAP, 5u, 0u};
    off = put(off, hints, sizeof(hints));
    off = put(off, "\0asm", 4);
    for (uint32_t i = 0; i < naot; ++i) {
        uint16_t rec16[2] = {aot[i].machine, 0u};
        uint32_t size = (uint32_t)strlen(aot[i].bytes);
        off = put(off, rec16, sizeof(rec16));
        off = put(off, &size, sizeof(size));
    }
    for (uint32_t i = 0; i < naot; ++i) {
        off = put(off, aot[i].bytes, (uint32_t)strlen(aot[i].bytes));
    }
    if (sig != 0u) {
        /* A well-formed Ed25519 record whose signature verifies nothing. */
        uint16_t head[2] = {WASMOS_APP_SIG_ALG_ED25519, (uint16_t)(sig - WASMOS_APP_SIG_HEAD_LEN)};
        off = put(off, head, sizeof(head));
        for (uint32_t i = 4u; i < sig; ++i) {
            g_blob[off++] = (uint8_t)0xA5u;
        }
    }
    return off;
}

static int selects(const wasmos_app_desc_t* desc, uint16_t machine, const char* want) {
    const uint8_t* bytes = (const uint8_t*)"x";
    uint32_t size = 7;
    if (!want) {
        return wasmos_app_compiled_for(desc, machine, &bytes, &size) == -1 && bytes == 0 &&
               size == 0;
    }
    return wasmos_app_compiled_for(desc, machine, &bytes, &size) == 0 && size == strlen(want) &&
           memcmp(bytes, want, size) == 0;
}

/* A fat package gives each architecture its own binary. */
static int test_each_architecture_gets_its_own_binary(void) {
    const test_aot_t aot[] = {{EM_X86_64, "x86code"}, {EM_AARCH64, "a64"}};
    uint32_t len = build(2, aot, 2, 0);
    wasmos_app_desc_t desc;
    if (wasmos_app_parse(g_blob, len, &desc) != 0 || desc.compiled_count != 2) {
        return __LINE__;
    }
    if (!selects(&desc, EM_X86_64, "x86code") || !selects(&desc, EM_AARCH64, "a64")) {
        return __LINE__;
    }
    if (desc.wasm_size != 4 || memcmp(desc.wasm_bytes, "\0asm", 4) != 0) {
        return __LINE__;
    }
    return 0;
}

/* A package with a binary for another architecture only yields none for this
 * one: the binary is never handed over, the bytecode still is. */
static int test_a_binary_for_another_architecture_is_not_selected(void) {
    const test_aot_t aot[] = {{EM_X86_64, "x86code"}};
    uint32_t len = build(1, aot, 1, 0);
    wasmos_app_desc_t desc;
    if (wasmos_app_parse(g_blob, len, &desc) != 0 || desc.compiled_count != 1) {
        return __LINE__;
    }
    if (!selects(&desc, EM_AARCH64, 0) || !selects(&desc, EM_X86_64, "x86code")) {
        return __LINE__;
    }
    if (desc.wasm_size != 4) {
        return __LINE__;
    }
    return 0;
}

static int test_no_binaries(void) {
    uint32_t len = build(0, 0, 0, 0);
    wasmos_app_desc_t desc;
    if (wasmos_app_parse(g_blob, len, &desc) != 0 || desc.compiled_count != 0 ||
        !selects(&desc, EM_X86_64, 0) || !selects(&desc, EM_AARCH64, 0)) {
        return __LINE__;
    }
    return 0;
}

/* The signature follows the last binary and ends the package. */
static int test_the_signature_follows_the_binaries(void) {
    const test_aot_t aot[] = {{EM_X86_64, "x86code"}, {EM_AARCH64, "a64"}};
    uint32_t len = build(2, aot, 2, SIG_RECORD_LEN);
    wasmos_app_desc_t desc;
    if (wasmos_app_parse(g_blob, len, &desc) != 0) {
        return __LINE__;
    }
    if (desc.signature != g_blob + len - WASMOS_APP_SIG_ED25519_LEN ||
        desc.signature_len != WASMOS_APP_SIG_ED25519_LEN ||
        desc.signature_alg != WASMOS_APP_SIG_ALG_ED25519 ||
        desc.signed_size != len - SIG_RECORD_LEN) {
        return __LINE__;
    }
    return 0;
}

static int test_two_binaries_for_one_architecture_are_refused(void) {
    const test_aot_t aot[] = {{EM_AARCH64, "one"}, {EM_AARCH64, "two"}};
    uint32_t len = build(2, aot, 2, 0);
    wasmos_app_desc_t desc;
    if (wasmos_app_parse(g_blob, len, &desc) == 0) {
        return __LINE__;
    }
    return 0;
}

static int test_an_untagged_or_empty_record_is_refused(void) {
    const test_aot_t untagged[] = {{0u, "code"}};
    wasmos_app_desc_t desc;
    uint32_t len = build(1, untagged, 1, 0);
    if (wasmos_app_parse(g_blob, len, &desc) == 0) {
        return __LINE__;
    }
    const test_aot_t empty[] = {{EM_X86_64, ""}};
    len = build(1, empty, 1, 0);
    if (wasmos_app_parse(g_blob, len, &desc) == 0) {
        return __LINE__;
    }
    return 0;
}

static int test_count_above_capacity_is_refused(void) {
    const test_aot_t aot[] = {{1u, "a"}, {2u, "b"}, {3u, "c"}, {4u, "d"}, {5u, "e"}};
    wasmos_app_desc_t desc;
    uint32_t len = build(WASMOS_APP_MAX_COMPILED, aot, WASMOS_APP_MAX_COMPILED, 0);
    if (wasmos_app_parse(g_blob, len, &desc) != 0 ||
        desc.compiled_count != WASMOS_APP_MAX_COMPILED) {
        return __LINE__;
    }
    len = build(WASMOS_APP_MAX_COMPILED + 1, aot, WASMOS_APP_MAX_COMPILED + 1, 0);
    if (wasmos_app_parse(g_blob, len, &desc) == 0) {
        return __LINE__;
    }
    return 0;
}

/* A blob cut anywhere inside the table or a binary is refused. */
static int test_truncated_table_or_binary_is_refused(void) {
    const test_aot_t aot[] = {{EM_X86_64, "x86code"}, {EM_AARCH64, "a64"}};
    uint32_t len = build(2, aot, 2, 0);
    uint32_t payload_end = len - 2 * 8 - 7 - 3;
    wasmos_app_desc_t desc;
    for (uint32_t cut = payload_end; cut < len; ++cut) {
        if (wasmos_app_parse(g_blob, cut, &desc) == 0) {
            return __LINE__;
        }
    }
    if (wasmos_app_parse(g_blob, len, &desc) != 0) {
        return __LINE__;
    }
    return 0;
}

/* The test keys (tests/unit/stubs_signing_pubkey.c): the seed's first byte, and
 * the id the record names. */
#define SYSTEM_SEED 0x00u
#define APP_SEED 0x20u
#define STRANGER_SEED 0x80u
extern const uint8_t wasmos_image_signing_key_id[WASMOS_APP_SIG_KEY_ID_LEN];
/* The id of the key from seed 20 21 22 ... 3f: a valid key the kernel does not
 * compile in (as the build's app key, which only the keyring registers). */
static const uint8_t k_app_key_id[WASMOS_APP_SIG_KEY_ID_LEN] = {
    0xc1, 0xbb, 0x02, 0xc0, 0xdb, 0x0b, 0x56, 0xb7, 0x57, 0x23, 0x21, 0x4c, 0x0c, 0xc6, 0x13, 0x02};

/* Sign the package build() left in g_blob as the packer does -- declare the
 * record in the header, sign every byte before it, append the record -- with the
 * key from seed `first`, first+1, ... and naming `key_id`. Returns the new
 * length. */
static uint32_t sign_blob_with(uint32_t len, uint8_t first, const uint8_t* key_id) {
    uint8_t seed[32];
    uint8_t secret[64];
    uint8_t pub[32];
    for (uint32_t i = 0; i < 32; ++i) {
        seed[i] = (uint8_t)(first + i);
    }
    crypto_ed25519_key_pair(secret, pub, seed);
    test_header_t hdr;
    memcpy(&hdr, g_blob, sizeof(hdr));
    hdr.signature_size = SIG_RECORD_LEN;
    memcpy(g_blob, &hdr, sizeof(hdr));
    uint16_t head[2] = {WASMOS_APP_SIG_ALG_ED25519, WASMOS_APP_SIG_ED25519_LEN};
    memcpy(&g_blob[len], head, sizeof(head));
    memcpy(&g_blob[len + 4u], key_id, WASMOS_APP_SIG_KEY_ID_LEN);
    crypto_ed25519_sign(&g_blob[len + WASMOS_APP_SIG_HEAD_LEN], secret, g_blob, len);
    return len + SIG_RECORD_LEN;
}

static uint32_t sign_blob(uint32_t len) {
    return sign_blob_with(len, SYSTEM_SEED, wasmos_image_signing_key_id);
}

static uint32_t select_for(const wasmos_app_desc_t* desc, uint16_t machine, const char* want) {
    const uint8_t* bytes = (const uint8_t*)"x";
    uint32_t size = 7;
    uint32_t status = wasmos_app_aot_select(desc, wasmos_app_trust(desc), machine, &bytes, &size);
    if (status == WASMOS_APP_AOT_SELECTED) {
        if (!want || size != strlen(want) || memcmp(bytes, want, size) != 0) {
            return 0xFFu;
        }
    } else if (bytes != 0 || size != 0) {
        return 0xFEu;
    }
    return status;
}

/* Regression: 2026-11-28-aot-unsigned -- the kernel ran any package's AOT
 * binary as native code. AOT code is trusted like a native image (it skips every
 * check WARP's compiler would emit), but an unsigned package could carry a
 * hand-made one tagged for this machine. Only a package whose signature the
 * kernel trusts has its binary used; any other runs from its bytecode. */
static int test_an_unsigned_package_runs_no_aot_binary(void) {
    const test_aot_t aot[] = {{EM_X86_64, "x86code"}, {EM_AARCH64, "a64"}};
    uint32_t len = build(2, aot, 2, 0);
    wasmos_app_desc_t desc;
    if (wasmos_app_parse(g_blob, len, &desc) != 0) {
        return __LINE__;
    }
    if (select_for(&desc, EM_X86_64, 0) != WASMOS_APP_AOT_UNTRUSTED ||
        select_for(&desc, EM_AARCH64, 0) != WASMOS_APP_AOT_UNTRUSTED) {
        return __LINE__;
    }
    return 0;
}

static int test_a_package_altered_after_signing_runs_no_aot_binary(void) {
    const test_aot_t aot[] = {{EM_X86_64, "x86code"}};
    uint32_t len = sign_blob(build(1, aot, 1, 0));
    g_blob[len - SIG_RECORD_LEN - 1u] ^= 0x01u; /* the binary's last byte */
    wasmos_app_desc_t desc;
    if (wasmos_app_parse(g_blob, len, &desc) != 0) {
        return __LINE__;
    }
    if (select_for(&desc, EM_X86_64, 0) != WASMOS_APP_AOT_UNTRUSTED) {
        return __LINE__;
    }
    return 0;
}

static int test_a_signed_package_runs_its_own_binary_only(void) {
    const test_aot_t aot[] = {{EM_X86_64, "x86code"}, {EM_AARCH64, "a64"}};
    uint32_t len = sign_blob(build(2, aot, 2, 0));
    wasmos_app_desc_t desc;
    if (wasmos_app_parse(g_blob, len, &desc) != 0) {
        return __LINE__;
    }
    if (select_for(&desc, EM_X86_64, "x86code") != WASMOS_APP_AOT_SELECTED ||
        select_for(&desc, EM_AARCH64, "a64") != WASMOS_APP_AOT_SELECTED ||
        select_for(&desc, 0x1234u, 0) != WASMOS_APP_AOT_OTHER_ARCH) {
        return __LINE__;
    }
    uint32_t plain = build(0, 0, 0, 0);
    if (wasmos_app_parse(g_blob, sign_blob(plain), &desc) != 0 ||
        select_for(&desc, EM_X86_64, 0) != WASMOS_APP_AOT_ABSENT) {
        return __LINE__;
    }
    return 0;
}

/* Which key signed a package decides what it may do; a signature no trusted key
 * verifies refuses the start. Regression: 2026-11-28-signed-invalid-starts -- a
 * package that carried a signature which did not verify (altered, or signed with
 * a key this kernel does not know) still started, merely unprivileged. */
static int test_the_signing_key_decides_the_trust(void) {
    const test_aot_t aot[] = {{EM_X86_64, "x86code"}};
    static const uint8_t unknown_id[WASMOS_APP_SIG_KEY_ID_LEN] = {0xEE};
    wasmos_app_desc_t desc;
    const struct {
        uint8_t seed;
        const uint8_t* key_id;
        uint32_t trust;
        int admitted;
        int system;
        uint32_t aot;
    } cases[] = {
        {SYSTEM_SEED,
         wasmos_image_signing_key_id,
         WASMOS_APP_TRUST_SYSTEM,
         1,
         1,
         WASMOS_APP_AOT_SELECTED},
        /* A correctly signed package by a key the kernel was not given is
         * unknown, so invalid: only the keyring can vouch for it. */
        {APP_SEED, k_app_key_id, WASMOS_APP_TRUST_INVALID, 0, 0, WASMOS_APP_AOT_UNTRUSTED},
        {STRANGER_SEED, unknown_id, WASMOS_APP_TRUST_INVALID, 0, 0, WASMOS_APP_AOT_UNTRUSTED},
        /* A stranger naming a trusted key's id is checked against that key. */
        {STRANGER_SEED,
         wasmos_image_signing_key_id,
         WASMOS_APP_TRUST_INVALID,
         0,
         0,
         WASMOS_APP_AOT_UNTRUSTED},
        {APP_SEED,
         wasmos_image_signing_key_id,
         WASMOS_APP_TRUST_INVALID,
         0,
         0,
         WASMOS_APP_AOT_UNTRUSTED},
    };
    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint32_t len = sign_blob_with(build(1, aot, 1, 0), cases[i].seed, cases[i].key_id);
        if (wasmos_app_parse(g_blob, len, &desc) != 0) {
            return __LINE__;
        }
        if (wasmos_app_trust(&desc) != cases[i].trust ||
            (wasmos_app_admit(&desc) == 0) != cases[i].admitted ||
            wasmos_app_signature_valid(&desc) != cases[i].system ||
            select_for(&desc, EM_X86_64, "x86code") != cases[i].aot) {
            return __LINE__;
        }
    }
    return 0;
}

static int test_an_unsigned_package_is_admitted_untrusted(void) {
    const test_aot_t aot[] = {{EM_X86_64, "x86code"}};
    wasmos_app_desc_t desc;
    uint32_t len = build(1, aot, 1, 0);
    if (wasmos_app_parse(g_blob, len, &desc) != 0 ||
        wasmos_app_trust(&desc) != WASMOS_APP_TRUST_UNSIGNED || wasmos_app_admit(&desc) != 0 ||
        wasmos_app_signature_valid(&desc) != 0) {
        return __LINE__;
    }
    return 0;
}

static int test_an_altered_signed_package_is_refused(void) {
    const test_aot_t aot[] = {{EM_X86_64, "x86code"}};
    wasmos_app_desc_t desc;
    uint32_t len = sign_blob(build(1, aot, 1, 0));
    g_blob[len - SIG_RECORD_LEN - 1u] ^= 0x01u;
    if (wasmos_app_parse(g_blob, len, &desc) != 0 ||
        wasmos_app_trust(&desc) != WASMOS_APP_TRUST_INVALID || wasmos_app_admit(&desc) == 0) {
        return __LINE__;
    }
    return 0;
}

/* The record's head must describe what follows it. An RSA-PSS record parses
 * (its signature is the keyring's to check), and is invalid until one does. */
static int test_signature_records_are_checked(void) {
    const test_aot_t aot[] = {{EM_X86_64, "x86code"}};
    wasmos_app_desc_t desc;
    uint32_t len = sign_blob(build(1, aot, 1, 0));
    uint32_t rec = len - SIG_RECORD_LEN;
    uint16_t alg = 7u;
    memcpy(&g_blob[rec], &alg, sizeof(alg));
    if (wasmos_app_parse(g_blob, len, &desc) == 0) {
        return __LINE__; /* unknown algorithm */
    }
    len = sign_blob(build(1, aot, 1, 0));
    uint16_t short_len = 32u;
    memcpy(&g_blob[rec + 2u], &short_len, sizeof(short_len));
    if (wasmos_app_parse(g_blob, len, &desc) == 0) {
        return __LINE__; /* length disagrees with the header */
    }
    uint32_t base = build(1, aot, 1, 0);
    test_header_t hdr;
    memcpy(&hdr, g_blob, sizeof(hdr));
    hdr.signature_size = WASMOS_APP_SIG_HEAD_LEN + 256u;
    memcpy(g_blob, &hdr, sizeof(hdr));
    uint16_t head[2] = {WASMOS_APP_SIG_ALG_RSA_PSS_SHA512, 256u};
    memcpy(&g_blob[base], head, sizeof(head));
    memset(&g_blob[base + 4u], 0x11, WASMOS_APP_SIG_KEY_ID_LEN + 256u);
    if (wasmos_app_parse(g_blob, base + WASMOS_APP_SIG_HEAD_LEN + 256u, &desc) != 0 ||
        desc.signature_alg != WASMOS_APP_SIG_ALG_RSA_PSS_SHA512 || desc.signature_len != 256u ||
        wasmos_app_trust(&desc) != WASMOS_APP_TRUST_INVALID) {
        return __LINE__;
    }
    return 0;
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_each_architecture_gets_its_own_binary),
        WASMOS_TEST_CASE(test_a_binary_for_another_architecture_is_not_selected),
        WASMOS_TEST_CASE(test_no_binaries),
        WASMOS_TEST_CASE(test_the_signature_follows_the_binaries),
        WASMOS_TEST_CASE(test_two_binaries_for_one_architecture_are_refused),
        WASMOS_TEST_CASE(test_an_untagged_or_empty_record_is_refused),
        WASMOS_TEST_CASE(test_count_above_capacity_is_refused),
        WASMOS_TEST_CASE(test_truncated_table_or_binary_is_refused),
        WASMOS_TEST_CASE(test_an_unsigned_package_runs_no_aot_binary),
        WASMOS_TEST_CASE(test_a_package_altered_after_signing_runs_no_aot_binary),
        WASMOS_TEST_CASE(test_a_signed_package_runs_its_own_binary_only),
        WASMOS_TEST_CASE(test_the_signing_key_decides_the_trust),
        WASMOS_TEST_CASE(test_an_unsigned_package_is_admitted_untrusted),
        WASMOS_TEST_CASE(test_an_altered_signed_package_is_refused),
        WASMOS_TEST_CASE(test_signature_records_are_checked),
    };
    const int groups = (int)(sizeof(cases) / sizeof(cases[0]));
    if (wasmos_test_run_all(cases, groups) != 0) {
        return 1;
    }
    printf("test_wasmos_app_aot: %d groups passed\n", groups);
    return 0;
}
