/* pl031.ts - the Arm PL031 real-time clock backend (aarch64), reached through a
 * memory window. The driver around it is src/drivers/rtc/rtc_core.ts; this
 * file is the chip.
 *
 * The PL031 is a free-running 32-bit counter of seconds (Arm PrimeCell RTC
 * PL031 TRM, DDI 0224, 3.3). RTCDR reads the count, a write to RTCLR loads it,
 * and the counter runs once RTCCR's start bit is set, which only a reset can
 * clear. The count is seconds since 1970-01-01 00:00:00 UTC, as QEMU and Linux
 * both keep it; this backend converts it to and from the protocol's date
 * (rtc_time.ts). The match register and its interrupt are not used: masked at
 * init, and the driver routes no IRQ.
 *
 * Every register is 32-bit and addressed through reg(): the window is granted
 * in whole pages, so a base that is not page-aligned sits inside region 0. The
 * device manager names the window in the spawn args (`mmio=`); there is no
 * default device. */

import {io, startup} from "./wasmos";
import {io_region_out32} from "./wasmos_imports";
import {WASMOS_ERR_DRIVER_DEVICE_INIT, WASMOS_ERR_NONE, WASMOS_ERR_RTC_IO} from "./wasmos_status";
import {runRtc, RtcClock} from "./rtc_core";
import {civilToSeconds, secondsToCivil} from "./rtc_time";

const REGION: i32 = 0;

const RTCDR: i32 = 0x000; /* data: the current count, read-only */
const RTCLR: i32 = 0x008; /* load: writing sets the count */
const RTCCR: i32 = 0x00c; /* control: bit 0 starts the counter */
const RTCIMSC: i32 = 0x010; /* interrupt mask set/clear */
const RTCICR: i32 = 0x01c; /* interrupt clear */
const RTC_PERIPH_ID0: i32 = 0xfe0;
const RTCCR_START: i64 = 1;

/* PrimeCell identification (TRM 3.3.9): part number 0x031 in PeriphID0 and the
 * low nibble of PeriphID1, designer 0x41 (Arm) across PeriphID1's high nibble
 * and PeriphID2's low nibble. The revision in PeriphID2's high nibble varies. */
const PART_PL031: i64 = 0x031;
const DESIGNER_ARM: i64 = 0x41;

/** The physical base named by the spawn args' `mmio=` (hex), or 0. */
function windowBase(): i64 {
    const args = startup.args();
    for (let i = 0; i < args.length; ++i) {
        const arg = unchecked(args[i]);
        if (arg.startsWith("mmio=")) {
            return <i64>I64.parseInt(arg.substring(5), 16);
        }
    }
    return 0;
}

class Pl031 extends RtcClock {
    base: i64 = 0;

    /* Offset in region 0 of the register at `offset` from the window's base. */
    reg(offset: i32): i32 {
        return <i32>(this.base & 0xfff) + offset;
    }

    label(): string {
        return "pl031";
    }

    /** One identification byte, or -1 when the read is refused. */
    idByte(index: i32): i64 {
        const v = io.regionIn32(REGION, this.reg(RTC_PERIPH_ID0 + index * 4));
        return v < 0 ? -1 : v & 0xff;
    }

    init(): i32 {
        this.base = windowBase();
        const id0 = this.idByte(0);
        const id1 = this.idByte(1);
        const id2 = this.idByte(2);
        if (id0 < 0 || id1 < 0 || id2 < 0) {
            return WASMOS_ERR_RTC_IO;
        }
        const part = id0 | ((id1 & 0x0f) << 8);
        const designer = (id1 >> 4) | ((id2 & 0x0f) << 4);
        if (part != PART_PL031 || designer != DESIGNER_ARM) {
            return WASMOS_ERR_DRIVER_DEVICE_INIT;
        }
        io_region_out32(REGION, this.reg(RTCIMSC), 0);
        io_region_out32(REGION, this.reg(RTCICR), 1);
        const cr = io.regionIn32(REGION, this.reg(RTCCR));
        if (cr < 0) {
            return WASMOS_ERR_RTC_IO;
        }
        if ((cr & RTCCR_START) == 0) {
            io_region_out32(REGION, this.reg(RTCCR), <i32>RTCCR_START);
        }
        return WASMOS_ERR_NONE;
    }

    read(out: StaticArray<i32>): i32 {
        const count = io.regionIn32(REGION, this.reg(RTCDR));
        if (count < 0) {
            return WASMOS_ERR_RTC_IO;
        }
        secondsToCivil(count, out);
        return WASMOS_ERR_NONE;
    }

    /* Every year the protocol accepts fits the 32-bit counter (2099 is below
     * its 2106 limit), so the load needs no range check of its own. */
    set(vals: StaticArray<i32>): i32 {
        if (io_region_out32(REGION, this.reg(RTCLR), <i32>civilToSeconds(vals)) != 0) {
            return WASMOS_ERR_RTC_IO;
        }
        return WASMOS_ERR_NONE;
    }
}

export function initialize(): i32 {
    return runRtc(new Pl031());
}
