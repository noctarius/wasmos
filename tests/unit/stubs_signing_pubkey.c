/* stubs_signing_pubkey.c - the package-signing public key for host tests.
 *
 * The kernel compiles in the key a build generates (make_wasmos_app --pubkey-c
 * from the build's seed). A host test needs a key it can also sign with, so
 * this is the public key of the fixed test seed 00 01 02 ... 1f
 * (TEST_SIGNING_SEED in the tests that sign), produced by that same command. */
#include <stdint.h>

const uint8_t wasmos_image_signing_pubkey[32] = {
    0x03, 0xa1, 0x07, 0xbf, 0xf3, 0xce, 0x10, 0xbe, 0x1d, 0x70, 0xdd, 0x18, 0xe7, 0x4b, 0xc0, 0x99,
    0x67, 0xe4, 0xd6, 0x30, 0x9b, 0xa5, 0x0d, 0x5f, 0x1d, 0xdc, 0x86, 0x64, 0x12, 0x55, 0x31, 0xb8};
