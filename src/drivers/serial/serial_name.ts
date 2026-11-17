/* The registry name a serial driver instance registers under, for the UART at
 * I/O base `port`, packed as the process manager unpacks a name from the
 * argument words of SVC_IPC_REGISTER_REQ: four bytes per word, little-endian,
 * NUL-padded.
 *
 * The console UART (COM1) is "sin", which is what the vt looks up. Every other
 * UART is "s" and its base in lowercase hex. */

/** COM1's base: the console UART. */
export const CONSOLE_PORT: i32 = 0x3f8;

/* "sin\0" -- serial input. */
const CONSOLE_NAME_PACKED: i32 = 0x006e6973;

/** Word `word` (0-3) of the packed name for the UART at `port`. */
export function serialNameWord(port: i32, word: i32): i32 {
    if (port == CONSOLE_PORT) {
        return word == 0 ? CONSOLE_NAME_PACKED : 0;
    }
    /* The whole 16-bit base, never a truncation of it: two UARTs whose bases
     * differ only above the low digits would otherwise share a name, and the
     * process manager refuses the second registration. At most "s" plus four
     * digits, which spills one byte into word 1. */
    const name = "s" + (port & 0xffff).toString(16);
    let packed: i32 = 0;
    for (let b = 0; b < 4; ++b) {
        const i = word * 4 + b;
        if (i < name.length) {
            packed |= (name.charCodeAt(i) & 0xff) << (b * 8);
        }
    }
    return packed;
}
