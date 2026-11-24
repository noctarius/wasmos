/* The registry name a serial driver instance registers under, packed as the
 * process manager unpacks a name from the argument words of
 * SVC_IPC_REGISTER_REQ: four bytes per word, little-endian, NUL-padded.
 *
 * The console UART is "sin", which is what the vt looks up. Every other UART is
 * "s" and the base of its register window in lowercase hex (an I/O port, or a
 * physical address for a memory-mapped UART). */

/** COM1's base: the console UART when the firmware names none. */
export const CONSOLE_PORT: i64 = 0x3f8;

/* "sin\0" -- serial input. */
const CONSOLE_NAME_PACKED: i32 = 0x006e6973;

/** Word `word` (0-3) of the packed name for the UART at `base`. */
export function serialNameWord(base: i64, console: bool, word: i32): i32 {
    if (console) {
        return word == 0 ? CONSOLE_NAME_PACKED : 0;
    }
    /* The whole base, never a truncation of it: two UARTs whose bases differ
     * only above the low digits would otherwise share a name, and the process
     * manager refuses the second registration. A 32-bit address is "s" plus
     * eight digits, which reaches into word 2; a register name holds up to 15
     * characters. */
    const name = "s" + base.toString(16);
    let packed: i32 = 0;
    for (let b = 0; b < 4; ++b) {
        const i = word * 4 + b;
        if (i < name.length) {
            packed |= (name.charCodeAt(i) & 0xff) << (b * 8);
        }
    }
    return packed;
}
