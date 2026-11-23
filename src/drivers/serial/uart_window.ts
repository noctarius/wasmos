/* uart_window.ts - where a UART register lies in its granted window.
 *
 * A memory-mapped register window is granted in whole pages (device-manager
 * rounds an MMIO window out to its page boundaries), so region 0 begins at the
 * page holding the window's base. */

/** Offset in region 0 of the register at `reg` from a window based at `base`:
 *  the base's displacement inside its page, plus `reg`. */
export function windowRegister(base: i64, reg: i32): i32 {
    return <i32>(base & 0xfff) + reg;
}
