import { putsn } from "../../../lib/libc/assemblyscript/wasmos";

let printed = false;

// Slightly more extensive AssemblyScript WASMOS-APP entry point.
export function main(
  _arg0: i32,
  _arg1: i32,
  _arg2: i32,
  _arg3: i32
): i32 {
  if (!printed) {
    printed = true;
    putsn("Hello from AssemblyScript on WASMOS!\n");
    putsn("This is a tiny WASMOS-APP written in AS.\n");
    putsn("Entry: main, runtime: stub\n");
  }
  return 0;
}
