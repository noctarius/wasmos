/* test_as_uart_window.ts - a UART register's offset in its granted window.
 *
 * Regression: 2026-11-23-pl011-subpage-window -- device-manager grants an MMIO
 * window rounded out to whole pages, but the PL011 backend addressed its
 * registers at bare offsets in that grant. For a firmware window at
 * 0x09000010 the flag register was read at 0x09000018 instead of 0x09000028,
 * and every initialisation write landed on the wrong register.
 */

import {windowRegister} from "./uart_window";
import {runShuffled, TestCase} from "./shuffle";

function testAPageAlignedWindowUsesTheBareOffset(): i32 {
    if (windowRegister(0x9000000, 0x18) != 0x18) return 101;
    return 0;
}

function testASubPageWindowKeepsItsDisplacement(): i32 {
    if (windowRegister(0x9000010, 0x18) != 0x28) return 201;
    if (windowRegister(0x9000ff0, 0x00) != 0xff0) return 202;
    return 0;
}

/** 0 if every case passed, else the failing case's marker. */
export function runTests(): i32 {
    const cases: StaticArray<TestCase> = [
        testAPageAlignedWindowUsesTheBareOffset,
        testASubPageWindowKeepsItsDisplacement,
    ];
    return runShuffled(cases);
}
