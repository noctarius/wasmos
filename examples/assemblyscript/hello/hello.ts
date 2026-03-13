import { fs, stdio } from "../../../lib/libc/assemblyscript/wasmos";

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
    const startup = fs.readTextFile("/startup.nsh");
    const readable = startup != null && startup.indexOf("BOOTX64.EFI") >= 0;
    stdio.println("Hello from AssemblyScript on WASMOS!");
    stdio.println("This is a tiny WASMOS-APP written in AS.");
    stdio.println("Entry: main, runtime: stub");
    stdio.println("startup.nsh readable: " + (readable ? "true" : "false"));
  }
  return 0;
}
