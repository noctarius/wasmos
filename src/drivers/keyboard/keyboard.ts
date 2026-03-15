const KEYBOARD_STATUS_PORT: i32 = 0x64;
const KEYBOARD_DATA_PORT: i32 = 0x60;
const KEYBOARD_OBF_FLAG: i32 = 0x01;

@external("wasmos", "console_write")
declare function console_write(ptr: i32, len: i32): i32;
@external("wasmos", "io_in8")
declare function io_in8(port: i32): i32;
@external("wasmos", "io_wait")
declare function io_wait(): i32;
@external("wasmos", "sched_yield")
declare function sched_yield(): i32;

function writeString(text: string): void {
  if (text.length == 0) {
    return;
  }
  let bytes = Uint8Array.wrap(String.UTF8.encode(text, false));
  console_write(bytes.dataStart as i32, bytes.byteLength as i32);
}

function readScancode(): i32 {
  if ((io_in8(KEYBOARD_STATUS_PORT) & KEYBOARD_OBF_FLAG) == 0) {
    return -1;
  }
  return io_in8(KEYBOARD_DATA_PORT) & 0xFF;
}

function formatScancode(code: i32): string {
  let hex = code.toString(16);
  if (hex.length == 1) {
    hex = "0" + hex;
  }
  return "0x" + hex.toUpperCase();
}

export function initialize(_proc_endpoint: i32, _module_count: i32, _arg2: i32, _arg3: i32): i32 {
  writeString("[keyboard] driver starting\n");

  for (;;) {
    let code = readScancode();
    if (code >= 0) {
      let msg = "[keyboard] scancode ";
      msg += formatScancode(code);
      msg += "\n";
      writeString(msg);
    }
    io_wait();
    sched_yield();
  }

  return 0;
}
