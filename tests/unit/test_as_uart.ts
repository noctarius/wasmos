/* test_as_uart.ts - the receive sequence the serial core runs against a chip.
 *
 * Regression: 2026-11-23-pl011-rx-clear-race -- the serial core emptied the
 * receive FIFO and THEN cleared the PL011's receive interrupt. A byte arriving
 * between the last read and the clear raised the interrupt and had it wiped at
 * once, so it sat in the FIFO with nothing pending; the PL011 raises the
 * interrupt again only for a new byte, and once the FIFO filled none could
 * arrive. The console stopped taking input with the system idle (seen as
 * test_supervisor stalling on aarch64: FR showed the FIFO full while RIS had no
 * receive bit and the GIC had nothing pending).
 */

import {RxSink, serviceRx, Uart} from "./uart";
import {runShuffled, TestCase} from "./shuffle";

/* A PL011's receive side: a FIFO and the raw receive interrupt status. A byte
 * arriving raises the status; ICR clears it; reading leaves it alone. When
 * `arriveAfterEmpty` is set, one byte arrives the first time the FIFO is found
 * empty -- the window between the drain's last read and what comes after it. */
class FakePl011 extends Uart {
    fifo: Array<i32> = [];
    rxStatus: bool = false;
    arriveAfterEmpty: bool = false;

    arrive(byte: i32): void {
        this.fifo.push(byte);
        this.rxStatus = true;
    }

    windowLabel(): string {
        return "fake";
    }
    defaultConsole(): bool {
        return false;
    }
    init(): void {}
    disableInterrupts(): void {}
    txReady(): bool {
        return true;
    }
    write(value: i32): void {}
    rxReady(): bool {
        if (this.fifo.length > 0) {
            return true;
        }
        if (this.arriveAfterEmpty) {
            this.arriveAfterEmpty = false;
            this.arrive(0x41);
        }
        return false;
    }
    read(): i32 {
        return this.fifo.shift();
    }
    clearRxInterrupt(): void {
        this.rxStatus = false;
    }
}

class CountSink extends RxSink {
    count: i32 = 0;
    push(byte: i32): void {
        this.count++;
    }
}

function testTheFifoIsEmptiedIntoTheSink(): i32 {
    const uart = new FakePl011();
    const sink = new CountSink();
    uart.arrive(1);
    uart.arrive(2);
    uart.arrive(3);
    serviceRx(uart, sink);
    if (sink.count != 3) return 101;
    if (uart.fifo.length != 0) return 102;
    return 0;
}

/* The invariant: when the sequence returns, a byte left in the FIFO still has
 * its receive interrupt raised, so it is serviced on the next interrupt. */
function testAByteArrivingDuringTheDrainKeepsItsInterrupt(): i32 {
    const uart = new FakePl011();
    const sink = new CountSink();
    uart.arrive(1);
    uart.arriveAfterEmpty = true;
    serviceRx(uart, sink);
    if (uart.fifo.length > 0 && !uart.rxStatus) return 201;
    return 0;
}

/** 0 if every case passed, else the failing case's marker. */
export function runTests(): i32 {
    const cases: StaticArray<TestCase> = [
        testTheFifoIsEmptiedIntoTheSink,
        testAByteArrivingDuringTheDrainKeepsItsInterrupt,
    ];
    return runShuffled(cases);
}
