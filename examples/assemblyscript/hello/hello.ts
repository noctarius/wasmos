import { fs, std } from "./wasmos";

let printed = false;

export function main(args: Array<string>): i32 {
  if (args.length != 0) {
    std.println("unexpected args");
  }
  if (!printed) {
    printed = true;
    const startup = fs.readTextFile("/startup.nsh");
    const readable = startup != null && startup.indexOf("BOOTX64.EFI") >= 0;
    std.println("Hello from AssemblyScript on WASMOS!");
    std.println("This is a tiny WASMOS-APP written in AS.");
    std.printf("Entry: main, runtime: stub\n");
    std.println("startup.nsh readable: " + (readable ? "true" : "false"));
  }
  return 0;
}
