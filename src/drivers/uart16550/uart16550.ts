/* uart16550.ts - the 16550 UART backend: x86_64 COM ports, reached through an
 * I/O window. The driver around it is serial_core.ts; this file is the chip.
 *
 * Registers are byte offsets in the window: +0 the receive/transmit holding
 * register (and, with DLAB set, the low divisor byte), +1 the Interrupt Enable
 * Register (high divisor byte under DLAB), +2 the FIFO control register, +3 the
 * line control register holding DLAB, +4 modem control, +5 the Line Status
 * Register. Only the three used repeatedly are named; the rest appear as bare
 * offsets in init(), where the write order matters more than the names. In the
 * line status register bit 0 (0x01) is "receive data available" and bit 5
 * (0x20) is "transmit holding register empty". */

import {io} from "./wasmos";
import {io_region_out8} from "./wasmos_imports";
import {REGION, runSerial, Uart} from "./serial_core";
import {CONSOLE_PORT} from "./serial_name";

const REG_IER: i32 = 1;
const REG_STATUS: i32 = 5;

/* COM1's IRQ; its base is CONSOLE_PORT. */
const CONSOLE_IRQ: i32 = 4;

class Uart16550 extends Uart {
    constructor() {
        super();
        this.base = CONSOLE_PORT;
        this.irq = CONSOLE_IRQ;
    }

    windowLabel(): string {
        return "io";
    }

    /* COM1 is the console when the firmware names none. */
    defaultConsole(): bool {
        return this.base == CONSOLE_PORT;
    }

    init(): void {
        io_region_out8(REGION, REG_IER, 0x00); /* disable interrupts during setup */
        io_region_out8(REGION, 3, 0x80); /* DLAB on */
        io_region_out8(REGION, 0, 0x01); /* divisor low (115200) */
        io_region_out8(REGION, 1, 0x00); /* divisor high */
        io_region_out8(REGION, 3, 0x03); /* 8N1, DLAB off */
        io_region_out8(REGION, 2, 0xc7); /* FIFO enable, clear, 14-byte threshold */
        io_region_out8(REGION, 4, 0x0b); /* DTR|RTS|OUT2 (OUT2 gates IRQ to the PIC) */
        io_region_out8(REGION, REG_IER, 0x01); /* enable "received data available" interrupt */
    }

    disableInterrupts(): void {
        io_region_out8(REGION, REG_IER, 0x00);
    }

    txReady(): bool {
        const status = io.regionIn8(REGION, REG_STATUS);
        return status >= 0 && (status & 0x20) != 0;
    }

    rxReady(): bool {
        const status = io.regionIn8(REGION, REG_STATUS);
        return status >= 0 && (status & 0x01) != 0;
    }

    write(value: i32): void {
        io_region_out8(REGION, 0, value);
    }

    read(): i32 {
        const byte = io.regionIn8(REGION, 0);
        return byte < 0 ? -1 : byte;
    }

    /* Reading the receive buffer clears the 16550's receive interrupt. */
    clearRxInterrupt(): void {}
}

export function initialize(): i32 {
    return runSerial(new Uart16550());
}
