import { std } from "./wasmos";

const CMOS_INDEX_PORT: i32 = 0x70;
const CMOS_DATA_PORT: i32 = 0x71;

const SVC_IPC_REGISTER_REQ: i32 = 0x220;
const SVC_IPC_REGISTER_RESP: i32 = 0x2A0;
const PROC_IPC_NOTIFY_READY: i32 = 0x20C;

const RTC_IPC_READ_REQ: i32 = 0x820;
const RTC_IPC_SET_REQ: i32 = 0x821;
const RTC_IPC_READ_RESP: i32 = 0x8A0;
const RTC_IPC_SET_RESP: i32 = 0x8A1;
const RTC_IPC_ERROR: i32 = 0x8FF;

const RTC_STATUS_OK: i32 = 0;
const RTC_STATUS_INVALID: i32 = -1;
const RTC_STATUS_IO: i32 = -2;
const RTC_STATUS_TIMEOUT: i32 = -3;

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
@external("wasmos", "sched_yield")
declare function sched_yield(): i32;

let g_rtc_ep: i32 = -1;

function ipcSendRetry(dest: i32, src: i32, type: i32, reqId: i32,
                      arg0: i32, arg1: i32, arg2: i32, arg3: i32): i32 {
  for (let i = 0; i < 512; ++i) {
    let rc = ipc_send(dest, src, type, reqId, arg0, arg1, arg2, arg3);
    if (rc == 0) {
      return 0;
    }
    sched_yield();
  }
  return -1;
}

function rtcReadReg(reg: i32): i32 {
  io_out8(CMOS_INDEX_PORT, reg & 0x7F);
  io_wait();
  return io_in8(CMOS_DATA_PORT) & 0xFF;
}

function rtcWriteReg(reg: i32, value: i32): void {
  io_out8(CMOS_INDEX_PORT, reg & 0x7F);
  io_wait();
  io_out8(CMOS_DATA_PORT, value & 0xFF);
  io_wait();
}

function bcdToBin(v: i32): i32 {
  return ((v >> 4) * 10) + (v & 0x0F);
}

function binToBcd(v: i32): i32 {
  return (((v / 10) & 0x0F) << 4) | (v % 10);
}

function waitNotUpdating(): bool {
  for (let i = 0; i < 10000; ++i) {
    let a = rtcReadReg(0x0A);
    if ((a & 0x80) == 0) {
      return true;
    }
    io_wait();
  }
  return false;
}

function unpackTime(arg0: i32, arg1: i32, outVals: StaticArray<i32>): void {
  unchecked(outVals[0] = arg0 & 0xFF);
  unchecked(outVals[1] = (arg0 >> 8) & 0xFF);
  unchecked(outVals[2] = (arg0 >> 16) & 0xFF);
  unchecked(outVals[3] = (arg0 >> 24) & 0xFF);
  unchecked(outVals[4] = arg1 & 0xFF);
  unchecked(outVals[5] = (arg1 >> 8) & 0xFFFF);
}

function packTime(outVals: StaticArray<i32>): i32 {
  return (unchecked(outVals[0]) & 0xFF) |
         ((unchecked(outVals[1]) & 0xFF) << 8) |
         ((unchecked(outVals[2]) & 0xFF) << 16) |
         ((unchecked(outVals[3]) & 0xFF) << 24);
}

function packDate(outVals: StaticArray<i32>): i32 {
  return (unchecked(outVals[4]) & 0xFF) |
         ((unchecked(outVals[5]) & 0xFFFF) << 8);
}

function validateTime(vals: StaticArray<i32>): bool {
  let sec = unchecked(vals[0]);
  let min = unchecked(vals[1]);
  let hour = unchecked(vals[2]);
  let day = unchecked(vals[3]);
  let mon = unchecked(vals[4]);
  let year = unchecked(vals[5]);
  return sec >= 0 && sec <= 59 &&
         min >= 0 && min <= 59 &&
         hour >= 0 && hour <= 23 &&
         day >= 1 && day <= 31 &&
         mon >= 1 && mon <= 12 &&
         year >= 1970 && year <= 2099;
}

function readTime(outVals: StaticArray<i32>): i32 {
  if (!waitNotUpdating()) {
    return RTC_STATUS_TIMEOUT;
  }

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
  return RTC_STATUS_OK;
}

function setTime(vals: StaticArray<i32>): i32 {
  if (!validateTime(vals)) {
    return RTC_STATUS_INVALID;
  }
  if (!waitNotUpdating()) {
    return RTC_STATUS_TIMEOUT;
  }

  let sec = unchecked(vals[0]);
  let min = unchecked(vals[1]);
  let hour = unchecked(vals[2]);
  let day = unchecked(vals[3]);
  let mon = unchecked(vals[4]);
  let fullYear = unchecked(vals[5]);
  let year = fullYear % 100;

  let regB = rtcReadReg(0x0B);
  let isBinary = (regB & 0x04) != 0;
  let is24Hour = (regB & 0x02) != 0;

  let hourReg = hour;
  if (!is24Hour) {
    let isPm = hour >= 12;
    let h12 = hour % 12;
    if (h12 == 0) {
      h12 = 12;
    }
    hourReg = h12;
    if (isPm) {
      hourReg |= 0x80;
    }
  }

  if (!isBinary) {
    sec = binToBcd(sec);
    min = binToBcd(min);
    day = binToBcd(day);
    mon = binToBcd(mon);
    year = binToBcd(year);
    if (is24Hour) {
      hourReg = binToBcd(hourReg);
    } else {
      let pmBit = hourReg & 0x80;
      hourReg = binToBcd(hourReg & 0x7F) | pmBit;
    }
  }

  rtcWriteReg(0x0B, regB | 0x80);
  rtcWriteReg(0x00, sec);
  rtcWriteReg(0x02, min);
  rtcWriteReg(0x04, hourReg);
  rtcWriteReg(0x07, day);
  rtcWriteReg(0x08, mon);
  rtcWriteReg(0x09, year);
  rtcWriteReg(0x0B, regB & 0x7F);
  return RTC_STATUS_OK;
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

  if (type == RTC_IPC_READ_REQ) {
    let vals = new StaticArray<i32>(6);
    let rc = readTime(vals);
    if (rc != RTC_STATUS_OK) {
      ipc_send(source, g_rtc_ep, RTC_IPC_ERROR, reqId, rc, 0, 0, 0);
      return;
    }
    ipc_send(source, g_rtc_ep, RTC_IPC_READ_RESP, reqId, packTime(vals), packDate(vals), 0, 0);
    return;
  }

  if (type == RTC_IPC_SET_REQ) {
    let vals = new StaticArray<i32>(6);
    unpackTime(ipc_last_field(2), ipc_last_field(3), vals);
    let rc = setTime(vals);
    if (rc != RTC_STATUS_OK) {
      ipc_send(source, g_rtc_ep, RTC_IPC_ERROR, reqId, rc, 0, 0, 0);
      return;
    }
    ipc_send(source, g_rtc_ep, RTC_IPC_SET_RESP, reqId, RTC_STATUS_OK, 0, 0, 0);
    return;
  }

  ipc_send(source, g_rtc_ep, RTC_IPC_ERROR, reqId, RTC_STATUS_INVALID, 0, 0, 0);
}

export function initialize(procEndpoint: i32, _arg1: i32, _arg2: i32, _arg3: i32): i32 {
  g_rtc_ep = ipc_create_endpoint();
  if (g_rtc_ep < 0) {
    std.printf("[rtc] endpoint failure\n");
    return -1;
  }

  let reqId = 1;
  let rtcName = 0x00637472; /* "rtc\\0" */
  if (ipcSendRetry(procEndpoint, g_rtc_ep, SVC_IPC_REGISTER_REQ, reqId, rtcName, 0, 0, 0) != 0) {
    std.printf("[rtc] register send failure\n");
    return -1;
  }
  if (ipc_recv(g_rtc_ep) != 1 ||
      ipc_last_field(0) != SVC_IPC_REGISTER_RESP ||
      ipc_last_field(1) != reqId ||
      ipc_last_field(2) != 0) {
    std.printf("[rtc] register failure\n");
    return -1;
  }

  std.printf("[rtc] driver ready\n");
  ipc_send(procEndpoint, g_rtc_ep, PROC_IPC_NOTIFY_READY, 0, 0, 0, 0, 0);

  for (;;) {
    handleMessage();
  }

  return 0;
}
