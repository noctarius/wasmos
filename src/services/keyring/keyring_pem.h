/* keyring_pem.h - the keyring's pure helpers: reading a PEM public key,
 * recognising its algorithm, deriving its key id, and picking key files out of a
 * directory listing. No IPC and no global state, so they are host-tested
 * (tests/unit/test_keyring_pem.c). */
#ifndef WASMOS_KEYRING_PEM_H
#define WASMOS_KEYRING_PEM_H

#include <stdint.h>

/* Length of a key id: the first 16 bytes of BLAKE2b-256 over the key's DER
 * SubjectPublicKeyInfo (WASMOS_APP_SIG_KEY_ID_LEN in the kernel). */
#define KEYRING_KEY_ID_LEN 16u

/* What a DER SubjectPublicKeyInfo holds. */
#define KEYRING_KEY_UNKNOWN 0
#define KEYRING_KEY_ED25519 1
#define KEYRING_KEY_RSA 2

/* Decode the first "PUBLIC KEY" PEM block in `pem` (`len` bytes, not
 * necessarily NUL-terminated) into its DER bytes at `der`. Returns the DER
 * length, or -1 when there is no complete block, its base64 is malformed, or it
 * does not fit `cap`. */
int32_t keyring_pem_to_der(const char* pem, uint32_t len, uint8_t* der, uint32_t cap);

/* KEYRING_KEY_ED25519 for the RFC 8410 Ed25519 SubjectPublicKeyInfo (44 bytes,
 * fixed prefix), KEYRING_KEY_RSA for an rsaEncryption one (OID
 * 1.2.840.113549.1.1.1 in the AlgorithmIdentifier), KEYRING_KEY_UNKNOWN
 * otherwise. Recognition only: an RSA key is parsed by mbedTLS where it is
 * used. */
int keyring_der_kind(const uint8_t* der, uint32_t len);

/* The key id of a DER SubjectPublicKeyInfo, as the kernel and the packer derive
 * it. */
void keyring_key_id(const uint8_t* der, uint32_t len, uint8_t id[KEYRING_KEY_ID_LEN]);

/* The next key file in a directory listing -- names separated by '\n', a
 * directory marked by a trailing '/' -- starting at *pos: copies a name ending
 * in ".pem" to `name` (NUL-terminated) and returns 1, skipping directories,
 * other files and names that do not fit `cap`; returns 0 at the end. */
int keyring_listing_next(const char* listing, uint32_t len, uint32_t* pos, char* name,
                         uint32_t cap);

#endif /* WASMOS_KEYRING_PEM_H */
