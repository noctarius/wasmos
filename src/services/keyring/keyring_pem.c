/* keyring_pem.c - see keyring_pem.h. */
#include "keyring_pem.h"

#include "monocypher.h"

static const char k_begin[] = "-----BEGIN PUBLIC KEY-----";
static const char k_end[] = "-----END PUBLIC KEY-----";

/* RFC 8410: SEQUENCE { SEQUENCE { OID 1.3.101.112 } BIT STRING (32 bytes) }. */
static const uint8_t k_ed25519_prefix[12] = {
    0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00};
/* The DER encoding of OID 1.2.840.113549.1.1.1 (rsaEncryption), tag and length
 * included. */
static const uint8_t k_rsa_oid[11] = {
    0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01};

static int32_t find(const char* hay, uint32_t len, uint32_t from, const char* needle,
                    uint32_t needle_len) {
    for (uint32_t i = from; i + needle_len <= len; ++i) {
        uint32_t j = 0;
        while (j < needle_len && hay[i + j] == needle[j]) {
            ++j;
        }
        if (j == needle_len) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

int32_t keyring_pem_to_der(const char* pem, uint32_t len, uint8_t* der, uint32_t cap) {
    if (!pem || !der) {
        return -1;
    }
    int32_t begin = find(pem, len, 0, k_begin, (uint32_t)(sizeof(k_begin) - 1u));
    if (begin < 0) {
        return -1;
    }
    uint32_t body = (uint32_t)begin + (uint32_t)(sizeof(k_begin) - 1u);
    int32_t end = find(pem, len, body, k_end, (uint32_t)(sizeof(k_end) - 1u));
    if (end < 0) {
        return -1;
    }
    uint32_t acc = 0;
    uint32_t bits = 0;
    uint32_t out = 0;
    uint32_t pad = 0;
    for (uint32_t i = body; i < (uint32_t)end; ++i) {
        char c = pem[i];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            continue;
        }
        if (c == '=') {
            ++pad;
            continue;
        }
        int v = b64_value(c);
        if (v < 0 || pad != 0) {
            return -1; /* not base64, or data after padding */
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out >= cap) {
                return -1;
            }
            der[out++] = (uint8_t)(acc >> bits);
        }
    }
    if (pad > 2 || out == 0) {
        return -1;
    }
    return (int32_t)out;
}

int keyring_der_kind(const uint8_t* der, uint32_t len) {
    if (!der) {
        return KEYRING_KEY_UNKNOWN;
    }
    if (len == sizeof(k_ed25519_prefix) + 32u) {
        uint32_t i = 0;
        while (i < sizeof(k_ed25519_prefix) && der[i] == k_ed25519_prefix[i]) {
            ++i;
        }
        if (i == sizeof(k_ed25519_prefix)) {
            return KEYRING_KEY_ED25519;
        }
    }
    /* An RSA SubjectPublicKeyInfo names rsaEncryption in its AlgorithmIdentifier,
     * the first element of the outer SEQUENCE: within its first few bytes. */
    for (uint32_t i = 0; i + sizeof(k_rsa_oid) <= len && i < 16u; ++i) {
        uint32_t j = 0;
        while (j < sizeof(k_rsa_oid) && der[i + j] == k_rsa_oid[j]) {
            ++j;
        }
        if (j == sizeof(k_rsa_oid)) {
            return KEYRING_KEY_RSA;
        }
    }
    return KEYRING_KEY_UNKNOWN;
}

void keyring_key_id(const uint8_t* der, uint32_t len, uint8_t id[KEYRING_KEY_ID_LEN]) {
    uint8_t hash[32];
    crypto_blake2b(hash, sizeof(hash), der, len);
    for (uint32_t i = 0; i < KEYRING_KEY_ID_LEN; ++i) {
        id[i] = hash[i];
    }
}

int keyring_listing_next(const char* listing, uint32_t len, uint32_t* pos, char* name,
                         uint32_t cap) {
    if (!listing || !pos || !name || cap == 0u) {
        return 0;
    }
    while (*pos < len) {
        uint32_t start = *pos;
        uint32_t end = start;
        while (end < len && listing[end] != '\n') {
            ++end;
        }
        *pos = end < len ? end + 1u : end;
        uint32_t n = end - start;
        while (n > 0 && (listing[start + n - 1u] == '\r' || listing[start + n - 1u] == ' ')) {
            --n;
        }
        if (n < 5u || n + 1u > cap || listing[start + n - 1u] == '/') {
            continue;
        }
        const char* tail = &listing[start + n - 4u];
        if (tail[0] != '.' || tail[1] != 'p' || tail[2] != 'e' || tail[3] != 'm') {
            continue;
        }
        for (uint32_t i = 0; i < n; ++i) {
            name[i] = listing[start + i];
        }
        name[n] = '\0';
        return 1;
    }
    return 0;
}
