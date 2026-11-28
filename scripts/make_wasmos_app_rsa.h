/* make_wasmos_app_rsa.h - make_wasmos_app's RSA operations (host mbedTLS). */
#ifndef WASMOS_MAKE_WASMOS_APP_RSA_H
#define WASMOS_MAKE_WASMOS_APP_RSA_H

#include <stdint.h>

/* Length in bytes of the signature the RSA private key in PEM file `key_path`
 * makes (its modulus length), or -1 when it cannot be read or is not an RSA key
 * of 2048 to 4096 bits. */
int rsa_signature_len(const char* key_path);

/* Sign the package written at `path`, whose header already declares a
 * signature_size of the record's length: RSASSA-PSS (SHA-512 hash and MGF1,
 * salt as long as the digest) over the SHA-512 digest of every byte, appended
 * as an algorithm-2 record naming the key's id (`key_id` fills it: BLAKE2b-256
 * of the DER SubjectPublicKeyInfo, first 16 bytes). Returns 0 or -1. */
int rsa_sign_package(const char* path, const char* key_path,
                     void (*key_id)(const uint8_t* der, uint32_t len, uint8_t id[16]));

/* Write a new RSA private key of `bits` bits to `out_path` as PEM. */
int rsa_genkey(const char* out_path, unsigned bits);

/* Write the public half of the PEM private key at `key_path` to `out_path` as a
 * PEM "PUBLIC KEY", the form the keyring reads. */
int rsa_pubkey_pem(const char* key_path, const char* out_path);

#endif /* WASMOS_MAKE_WASMOS_APP_RSA_H */
