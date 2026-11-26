/* crypto_smoke - an app using Monocypher from the SDK (-lmonocypher).
 *
 * Signs a message with Ed25519 from a fixed seed, checks the signature, checks
 * that a changed message no longer verifies, and hashes with BLAKE2b, then
 * reports "crypto-smoke: ok" -- or what failed. */
#include <stdint.h>

#include "monocypher.h"
#include "monocypher-ed25519.h"
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"

static void report(const char* what) {
    char line[64];
    (void)snprintf(line, sizeof(line), "crypto-smoke: %s\n", what);
    (void)wasmos_console_write(addr_cast(int32_t, line), (int32_t)strlen(line));
}

int main(void) {
    uint8_t seed[32];
    uint8_t secret[64];
    uint8_t pub[32];
    uint8_t sig[64];
    uint8_t msg[] = "signed in a wasmos app";
    uint8_t hash[64];
    for (uint32_t i = 0; i < sizeof(seed); ++i) {
        seed[i] = (uint8_t)(i * 7u + 1u);
    }
    crypto_ed25519_key_pair(secret, pub, seed);
    crypto_ed25519_sign(sig, secret, msg, sizeof(msg));
    if (crypto_ed25519_check(sig, pub, msg, sizeof(msg)) != 0) {
        report("a valid signature did not verify");
        return 1;
    }
    msg[0] ^= 0x01u;
    if (crypto_ed25519_check(sig, pub, msg, sizeof(msg)) == 0) {
        report("an altered message verified");
        return 1;
    }
    crypto_blake2b(hash, sizeof(hash), msg, sizeof(msg));
    crypto_wipe(secret, sizeof(secret));
    report("ok");
    return 0;
}
