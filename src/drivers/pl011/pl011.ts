/* pl011.ts - the Arm PL011 UART backend (aarch64), reached through a memory
 * window. The driver around it is serial_core.ts; this file is the chip.
 *
 * Registers are 32-bit, at byte offsets in the window (Arm PL011 TRM, DDI 0183,
 * 3.2). It has no default device: the device manager names its window and
 * interrupt in the spawn args. */

import {io} from "./wasmos";
import {io_region_out32} from "./wasmos_imports";
import {REGION, runSerial, Uart} from "./serial_core";

const DR: i32 = 0x00; /* data: bits 7:0 the byte */
const FR: i32 = 0x18; /* flags */
const LCR_H: i32 = 0x2c; /* line control */
const CR: i32 = 0x30; /* control */
const IMSC: i32 = 0x38; /* interrupt mask set/clear */
const ICR: i32 = 0x44; /* interrupt clear */
const FR_RXFE: i64 = 1 << 4; /* receive FIFO empty */
const FR_TXFF: i64 = 1 << 5; /* transmit FIFO full */
const LCR_H_8N1_FIFO: i32 = 0x70; /* WLEN 8 bits, FEN */
const CR_ENABLE: i32 = 0x301; /* UARTEN, TXE, RXE */
/* Receive and receive-timeout interrupts: the FIFO reached its level, or holds
 * bytes nobody has read for a while. */
const INT_RX: i32 = (1 << 4) | (1 << 6);
const INT_ALL: i32 = 0x7ff;

class Pl011 extends Uart {
    windowLabel(): string {
        return "pl011 mmio";
    }

    /* The console is whichever PL011 the firmware names (SPCR). */
    defaultConsole(): bool {
        return false;
    }

    /* The baud rate stays as the firmware programmed it: on the console the
     * kernel already writes through it. The line control may only change with
     * the UART disabled (PL011 TRM 3.3.8), so CR is cleared around it. */
    init(): void {
        io_region_out32(REGION, IMSC, 0);
        io_region_out32(REGION, CR, 0);
        io_region_out32(REGION, LCR_H, LCR_H_8N1_FIFO);
        io_region_out32(REGION, ICR, INT_ALL);
        io_region_out32(REGION, CR, CR_ENABLE);
        io_region_out32(REGION, IMSC, INT_RX);
    }

    disableInterrupts(): void {
        io_region_out32(REGION, IMSC, 0);
    }

    txReady(): bool {
        const flags = io.regionIn32(REGION, FR);
        return flags >= 0 && (flags & FR_TXFF) == 0;
    }

    rxReady(): bool {
        const flags = io.regionIn32(REGION, FR);
        return flags >= 0 && (flags & FR_RXFE) == 0;
    }

    write(value: i32): void {
        io_region_out32(REGION, DR, value);
    }

    read(): i32 {
        const data = io.regionIn32(REGION, DR);
        return data < 0 ? -1 : <i32>(data & 0xff);
    }

    /* The receive-timeout interrupt stays raised until cleared, even with the
     * FIFO drained. */
    clearRxInterrupt(): void {
        io_region_out32(REGION, ICR, INT_RX);
    }
}

export function initialize(): i32 {
    return runSerial(new Pl011());
}
