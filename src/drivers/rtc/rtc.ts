import { std } from "./wasmos";

const CMOS_INDEX_PORT: i32 = 0x70;
const CMOS_DATA_PORT: i32 = 0x71;

const PROC_IPC_NOTIFY_READY: i32 = 0x20C;

const RTC_IPC_READ_REQ: i32 = 0x820;
const RTC_IPC_READ_RESP: i32 = 0x8A0;
const RTC_IPC_ERROR: i32 = 0x8FF;
/* ABI contract:
 * READ_REQ: arg0..arg3 reserved.
 * READ_RESP: arg0=[sec|min|hour|day], arg1=[month|year16<<8]. */

@external("wasmos", "io_in8")
declare function io_in8(port: i32): i32;
@external("wasmos", "io_out8")
declare function io_out8(port: i32, value: i32): i32;
@external("wasmos", "io_wait")
declare function io_wait(): i32;
@external("wasmos", "ipc_recv")
declare function ipc_recv(endpoint: i32): i32;
@external("wasmos", "ipc_create_endpoint")
declare function ipc_create_endpoint(): i32;
@external("wasmos", "ipc_last_field")
declare function ipc_last_field(field: i32): i32;
@external("wasmos", "ipc_send")
declare function ipc_send(dest: i32, src: i32, type: i32, req_id: i32,
                          arg0: i32, arg1: i32, arg2: i32, arg3: i32): i32;

let g_rtc_ep: i32 = -1;

function rtcReadReg(reg: i32): i32 {
  io_out8(CMOS_INDEX_PORT, reg & 0x7F);
  io_wait();
  return io_in8(CMOS_DATA_PORT) & 0xFF;
}

function bcdToBin(v: i32): i32 {
  return ((v >> 4) * 10) + (v & 0x0F);
}

function waitNotUpdating(): void {
  for (let i = 0; i < 10000; ++i) {
    let a = rtcReadReg(0x0A);
    if ((a & 0x80) == 0) {
      return;
    }
    io_wait();
  }
}

function readTime(outVals: StaticArray<i32>): i32 {
  waitNotUpdating();

  let sec = rtcReadReg(0x00);
  let min = rtcReadReg(0x02);
  let hour = rtcReadReg(0x04);
  let day = rtcReadReg(0x07);
  let mon = rtcReadReg(0x08);
  let year = rtcReadReg(0x09);
  let regB = rtcReadReg(0x0B);

  let isBinary = (regB & 0x04) != 0;
  let is24Hour = (regB & 0x02) != 0;

  if (!isBinary) {
    sec = bcdToBin(sec);
    min = bcdToBin(min);
    let hourRaw = hour;
    hour = bcdToBin(hourRaw & 0x7F);
    if (!is24Hour && (hourRaw & 0x80) != 0) {
      hour = (hour + 12) % 24;
    }
    day = bcdToBin(day);
    mon = bcdToBin(mon);
    year = bcdToBin(year);
  } else if (!is24Hour && (hour & 0x80) != 0) {
    hour = ((hour & 0x7F) + 12) % 24;
  }

  let fullYear = 2000 + year;
  if (year >= 70) {
    fullYear = 1900 + year;
  }

  unchecked(outVals[0] = sec);
  unchecked(outVals[1] = min);
  unchecked(outVals[2] = hour);
  unchecked(outVals[3] = day);
  unchecked(outVals[4] = mon);
  unchecked(outVals[5] = fullYear);
  return 0;
}

function handleMessage(): void {
  if (ipc_recv(g_rtc_ep) != 1) {
    return;
  }

  let type = ipc_last_field(0);
  let reqId = ipc_last_field(1);
  let source = ipc_last_field(4);

  if (source < 0) {
    return;
  }

  if (type != RTC_IPC_READ_REQ) {
    ipc_send(source, g_rtc_ep, RTC_IPC_ERROR, reqId, -1, 0, 0, 0);
    return;
  }

  let vals = new StaticArray<i32>(6);
  if (readTime(vals) != 0) {
    ipc_send(source, g_rtc_ep, RTC_IPC_ERROR, reqId, -2, 0, 0, 0);
    return;
  }

  let a0 = (unchecked(vals[0]) & 0xFF) |
           ((unchecked(vals[1]) & 0xFF) << 8) |
           ((unchecked(vals[2]) & 0xFF) << 16) |
           ((unchecked(vals[3]) & 0xFF) << 24);
  let a1 = (unchecked(vals[4]) & 0xFF) | ((unchecked(vals[5]) & 0xFFFF) << 8);
  ipc_send(source, g_rtc_ep, RTC_IPC_READ_RESP, reqId, a0, a1, 0, 0);
}

export function initialize(procEndpoint: i32, _arg1: i32, _arg2: i32, _arg3: i32): i32 {
  g_rtc_ep = ipc_create_endpoint();
  if (g_rtc_ep < 0) {
    std.printf("[rtc] endpoint failure\n");
    return -1;
  }

  /* TODO(rtc-service-registration): register a stable public endpoint once
   * rtc client ABI and service naming are finalized. */
  std.printf("[rtc] driver ready\n");
  ipc_send(procEndpoint, g_rtc_ep, PROC_IPC_NOTIFY_READY, 0, 0, 0, 0, 0);

  for (;;) {
    handleMessage();
  }

  return 0;
}
