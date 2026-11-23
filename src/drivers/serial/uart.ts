/* uart.ts - the chip interface a UART backend implements, and the receive
 * sequence the serial core runs against it. No host calls: the sequence is
 * exercised against a fake chip by tests/unit/test_as_uart.ts.
 */

/* Every register is addressed in the UART's granted window, region 0, so a
 * backend never names an absolute port or address (io_region_* host calls). */
export const REGION: i32 = 0;

/**
 * One UART chip. A backend sets `base` and `irq` to its default device, and the
 * spawn args override them. Status reads that the kernel refuses report neither
 * ready: the register cannot answer, so claiming either bit would be inventing
 * hardware state.
 */
export abstract class Uart {
    /* The register window's base: an I/O port or a physical address. It names
     * the instance; registers are reached through REGION. */
    base: i64 = 0;
    irq: i32 = -1;
    /** How the boot log labels the window's base ("io", "pl011 mmio"). */
    abstract windowLabel(): string;
    /** Whether this UART is the console when the firmware names none. */
    abstract defaultConsole(): bool;
    /** Program the line and enable the receive interrupt. */
    abstract init(): void;
    /** Mask every device interrupt; the core then polls. */
    abstract disableInterrupts(): void;
    abstract txReady(): bool;
    abstract rxReady(): bool;
    /** Send one byte; called only when txReady(). */
    abstract write(value: i32): void;
    /** The next received byte, or -1 when the read is refused. */
    abstract read(): i32;
    /** Clear a receive interrupt draining the FIFO does not clear. */
    abstract clearRxInterrupt(): void;
}

/** Where serviceRx puts the bytes it reads. */
export abstract class RxSink {
    abstract push(byte: i32): void;
}

/**
 * Service a receive interrupt: clear the receive interrupt, then empty the
 * receive FIFO into `sink`.
 *
 * The clear comes FIRST. A PL011 raises its receive interrupt only when a byte
 * arrives, and ICR clears it whatever the FIFO holds; cleared after the drain,
 * a byte that arrived between the last read and the clear would stay in the
 * FIFO with nothing pending, and once the FIFO filled no new byte could arrive
 * to raise it again. Cleared first, any byte arriving after the clear raises
 * the interrupt anew and is serviced on the next one.
 */
export function serviceRx(uart: Uart, sink: RxSink): void {
    uart.clearRxInterrupt();
    for (;;) {
        if (!uart.rxReady()) {
            break;
        }
        const byte = uart.read();
        if (byte < 0) {
            break;
        }
        sink.push(byte);
    }
}
