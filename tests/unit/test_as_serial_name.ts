/* test_as_serial_name.ts — the registry name each serial driver instance takes.
 *
 * Regression: 2026-11-17-serial-name-masked-to-12-bits -- the name was built from
 * the low 12 bits of the UART's base, so UARTs at 0x02F8 and 0x12F8 both
 * registered as "s2f8". The process manager refuses a name another live process
 * holds, so the second instance failed its registration, died before it was
 * ready, and the device manager gave up on that UART.
 */

import {CONSOLE_PORT, serialNameWord} from "./serial_name";
import {runShuffled, TestCase} from "./shuffle";

/* The name the four packed words spell, as the process manager unpacks it. */
function unpack(base: i64, console: bool = false): string {
    let out = "";
    for (let w = 0; w < 4; ++w) {
        const word = serialNameWord(base, console, w);
        for (let b = 0; b < 4; ++b) {
            const c = (word >>> (b * 8)) & 0xff;
            if (c == 0) {
                return out;
            }
            out += String.fromCharCode(c);
        }
    }
    return out;
}

function testTheConsoleIsSin(): i32 {
    if (unpack(CONSOLE_PORT, true) != "sin") return 101;
    /* Whatever its base: a PL011 the firmware names as the console. */
    if (unpack(0x9000000, true) != "sin") return 102;
    return 0;
}

function testCom1IsNamedByItsBaseWhenItIsNotTheConsole(): i32 {
    if (unpack(CONSOLE_PORT) != "s3f8") return 401;
    return 0;
}

/* A memory-mapped UART is named by its physical address, all of it: a 32-bit
 * address spills into the third packed word. */
function testAMemoryMappedUartIsNamedByItsAddress(): i32 {
    if (unpack(0x9000000) != "s9000000") return 501;
    if (unpack(0xfe215040) != "sfe215040") return 502;
    if (unpack(0xfe215040) == unpack(0x215040)) return 503;
    return 0;
}

function testAnotherUartIsNamedByItsBase(): i32 {
    if (unpack(0x2f8) != "s2f8") return 201;
    if (unpack(0x3e8) != "s3e8") return 202;
    return 0;
}

function testBasesDifferingAboveTwelveBitsGetDifferentNames(): i32 {
    if (unpack(0x12f8) != "s12f8") return 301;
    if (unpack(0x12f8) == unpack(0x2f8)) return 302;
    if (unpack(0xfff8) != "sfff8") return 303;
    return 0;
}

/** 0 if every case passed, else the failing case's marker. */
export function runTests(): i32 {
    const cases: StaticArray<TestCase> = [
        testTheConsoleIsSin,
        testAnotherUartIsNamedByItsBase,
        testBasesDifferingAboveTwelveBitsGetDifferentNames,
        testCom1IsNamedByItsBaseWhenItIsNotTheConsole,
        testAMemoryMappedUartIsNamedByItsAddress,
    ];
    return runShuffled(cases);
}
