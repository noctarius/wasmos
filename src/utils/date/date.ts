import { ipc, startup, std } from "./wasmos";

const SVC_IPC_LOOKUP_REQ: i32 = 0x221;
const SVC_IPC_LOOKUP_RESP: i32 = 0x2A1;

const RTC_IPC_READ_REQ: i32 = 0x820;
const RTC_IPC_SET_REQ: i32 = 0x821;
const RTC_IPC_READ_RESP: i32 = 0x8A0;
const RTC_IPC_SET_RESP: i32 = 0x8A1;
const RTC_IPC_ERROR: i32 = 0x8FF;

@external("wasmos", "fs_buffer_copy")
declare function fs_buffer_copy(ptr: i32, len: i32, offset: i32): i32;

class RtcTime {
  constructor(
    public year: i32 = 1970,
    public month: i32 = 1,
    public day: i32 = 1,
    public hour: i32 = 0,
    public minute: i32 = 0,
    public second: i32 = 0
  ) {}
}

function packName16(name: string): StaticArray<i32> {
  const out = new StaticArray<i32>(4);
  for (let i = 0; i < name.length && i < 16; ++i) {
    const slot = i >> 2;
    const shift = (i & 3) << 3;
    const v = unchecked(out[slot]);
    unchecked(out[slot] = v | ((name.charCodeAt(i) & 0xFF) << shift));
  }
  return out;
}

function svcLookup(procEndpoint: i32, name: string): i32 {
  const packed = packName16(name);
  const reply = ipc.call(procEndpoint,
                         SVC_IPC_LOOKUP_REQ,
                         unchecked(packed[0]),
                         unchecked(packed[1]),
                         unchecked(packed[2]),
                         unchecked(packed[3]));
  if (reply == null || reply.type != SVC_IPC_LOOKUP_RESP) {
    return -1;
  }
  return reply.arg0;
}

function rtcRead(rtcEndpoint: i32): RtcTime | null {
  const reply = ipc.call(rtcEndpoint, RTC_IPC_READ_REQ, 0, 0, 0, 0);
  if (reply == null || reply.type == RTC_IPC_ERROR || reply.type != RTC_IPC_READ_RESP) {
    return null;
  }
  const arg0 = reply.arg0;
  const arg1 = reply.arg1;
  const t = new RtcTime();
  t.second = arg0 & 0xFF;
  t.minute = (arg0 >> 8) & 0xFF;
  t.hour = (arg0 >> 16) & 0xFF;
  t.day = (arg0 >> 24) & 0xFF;
  t.month = arg1 & 0xFF;
  t.year = (arg1 >> 8) & 0xFFFF;
  return t;
}

function rtcSet(rtcEndpoint: i32, t: RtcTime): bool {
  const arg0 = (t.second & 0xFF) |
               ((t.minute & 0xFF) << 8) |
               ((t.hour & 0xFF) << 16) |
               ((t.day & 0xFF) << 24);
  const arg1 = (t.month & 0xFF) | ((t.year & 0xFFFF) << 8);
  const reply = ipc.call(rtcEndpoint, RTC_IPC_SET_REQ, arg0, arg1, 0, 0);
  return reply != null && reply.type == RTC_IPC_SET_RESP && reply.arg0 == 0;
}

function pad2(v: i32): string {
  if (v < 10) {
    return "0" + v.toString();
  }
  return v.toString();
}

function formatTime(t: RtcTime): string {
  return t.year.toString() + "-" + pad2(t.month) + "-" + pad2(t.day) +
         " " + pad2(t.hour) + ":" + pad2(t.minute) + ":" + pad2(t.second);
}

function parse2(s: string, pos: i32): i32 {
  if (pos + 1 >= s.length) {
    return -1;
  }
  const c0 = s.charCodeAt(pos);
  const c1 = s.charCodeAt(pos + 1);
  if (c0 < 48 || c0 > 57 || c1 < 48 || c1 > 57) {
    return -1;
  }
  return (c0 - 48) * 10 + (c1 - 48);
}

function parse4(s: string, pos: i32): i32 {
  if (pos + 3 >= s.length) {
    return -1;
  }
  let v = 0;
  for (let i = 0; i < 4; ++i) {
    const c = s.charCodeAt(pos + i);
    if (c < 48 || c > 57) {
      return -1;
    }
    v = v * 10 + (c - 48);
  }
  return v;
}

function parseSetArg(args: string): RtcTime | null {
  const trimmed = args.trim();
  if (!trimmed.startsWith("set ")) {
    return null;
  }
  const s = trimmed.substring(4);
  if (s.length < 19 || s.charCodeAt(4) != 45 || s.charCodeAt(7) != 45 ||
      s.charCodeAt(10) != 32 || s.charCodeAt(13) != 58 || s.charCodeAt(16) != 58) {
    return null;
  }
  const t = new RtcTime();
  t.year = parse4(s, 0);
  t.month = parse2(s, 5);
  t.day = parse2(s, 8);
  t.hour = parse2(s, 11);
  t.minute = parse2(s, 14);
  t.second = parse2(s, 17);
  if (t.year < 1970 || t.year > 2099 ||
      t.month < 1 || t.month > 12 ||
      t.day < 1 || t.day > 31 ||
      t.hour < 0 || t.hour > 23 ||
      t.minute < 0 || t.minute > 59 ||
      t.second < 0 || t.second > 59) {
    return null;
  }
  return t;
}

function readSpawnArgs(): string {
  const buf = new Uint8Array(128);
  if (fs_buffer_copy(buf.dataStart as i32, buf.length - 1, 0) != 0) {
    return "";
  }
  let n = 0;
  while (n < buf.length && buf[n] != 0) {
    n++;
  }
  if (n == 0) {
    return "";
  }
  return String.UTF8.decodeUnsafe(buf.dataStart, n, false);
}

export function main(_args: Array<string>): i32 {
  const procEndpoint = startup.arg(0);
  if (procEndpoint <= 0) {
    std.println("date: missing proc endpoint");
    return 1;
  }

  const rtcEndpoint = svcLookup(procEndpoint, "rtc");
  if (rtcEndpoint < 0) {
    std.println("date: rtc service not found");
    return 1;
  }

  const spawnArgs = readSpawnArgs();
  if (spawnArgs.length > 0) {
    const t = parseSetArg(spawnArgs);
    if (t == null) {
      std.println("usage: date set YYYY-MM-DD HH:MM:SS");
      return 1;
    }
    if (!rtcSet(rtcEndpoint, t)) {
      std.println("date: set failed");
      return 1;
    }
  }

  const now = rtcRead(rtcEndpoint);
  if (now == null) {
    std.println("date: read failed");
    return 1;
  }
  std.println(formatTime(now));
  return 0;
}
