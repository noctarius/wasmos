/* keyring_pem.h - the keyring's pure helpers: reading a PEM public key,
 * recognising its algorithm, deriving its key id, picking key files out of a
 * directory listing, and reading and writing the package allowlist. No IPC and no global state, so
 * they are host-tested (tests/unit/test_keyring_pem.c). */
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

/* Length of a package digest on the allowlist (SHA-512 of the whole package
 * file, WASMOS_APP_PACKAGE_HASH_LEN in the kernel), and the longest package path
 * recorded beside one. */
#define KEYRING_DIGEST_LEN 64u
#define KEYRING_ALLOW_PATH_MAX 128u
/* The longest allowlist line: the digest in hex, a space, the path, '\n'. */
#define KEYRING_ALLOW_LINE_MAX (2u * KEYRING_DIGEST_LEN + 1u + KEYRING_ALLOW_PATH_MAX + 1u)

/* The next digest of an allowlist (allow.lst) starting at *pos: each line is a
 * digest as 128 hex digits, optionally followed by a space or tab and the package
 * it was taken from. Blank lines and lines starting with '#' are skipped. Returns
 * 1 with the digest in `digest`, 0 at the end, or -1 for a line that is none of
 * these (*pos is past it either way, so the caller may go on). */
int keyring_allow_next(const char* text, uint32_t len, uint32_t* pos,
                       uint8_t digest[KEYRING_DIGEST_LEN]);

/* The allowlist line for `digest` taken from the package at `path` (`path_len`
 * bytes, not NUL-terminated; a control character in it is written as '?', so the
 * line stays one line), written to `out` with a final '\n' and no NUL. Returns its
 * length, or -1 when the path is longer than KEYRING_ALLOW_PATH_MAX or the line
 * does not fit `cap`. */
int32_t keyring_allow_line(const uint8_t digest[KEYRING_DIGEST_LEN], const char* path,
                           uint32_t path_len, char* out, uint32_t cap);

/* The file a key added at run time is written to in the keyring directory: the
 * first 4 bytes of its id as 8 hex digits, ".pem" -- an 8.3 name, which every
 * FAT driver creates. `out` receives 13 bytes, the NUL included. */
void keyring_key_file_name(const uint8_t id[KEYRING_KEY_ID_LEN], char out[13]);

#endif /* WASMOS_KEYRING_PEM_H */
