export const WASMOS_WASM_STEP_YIELDED: i32 = 0;

@external("wasmos", "console_write")
declare function console_write(ptr: i32, len: i32): i32;

let printed = false;

function writeLine(msg: string): void {
  const buf = new Uint8Array(1);
  for (let i = 0; i < msg.length; i++) {
    buf[0] = msg.charCodeAt(i) as u8;
    console_write(buf.dataStart as i32, 1);
  }
}

// Slightly more extensive AssemblyScript WASMOS-APP entry point.
export function hello_step(
  _type: i32,
  _arg0: i32,
  _arg1: i32,
  _arg2: i32,
  _arg3: i32
): i32 {
  if (!printed) {
    printed = true;
    writeLine("Hello from AssemblyScript on WASMOS!\n");
    writeLine("This is a tiny WASMOS-APP written in AS.\n");
    writeLine("Entry: hello_step, runtime: stub\n");
  }
  return WASMOS_WASM_STEP_YIELDED;
}
