import { fs, std } from "./wasmos";

let printed = false;

export function main(args: Array<string>): i32 {
  if (args.length != 0) {
    std.println("unexpected args");
  }
  if (!printed) {
    const path = "/assemblyscript-long-file-check.txt";
    const content = String.UTF8.encode("assemblyscript shim long filename\n", false);
    const contentBytes = Uint8Array.wrap(content);
    let writeOk = false;
    const out = fs.create(path);
    if (out != null) {
      writeOk = out.write(contentBytes) == content.byteLength && out.close();
      if (writeOk) {
        const verify = fs.openRead(path);
        if (verify != null) {
          const readBack = verify.read(content.byteLength);
          writeOk = verify.close() && readBack != null && readBack.length == contentBytes.length;
          if (writeOk && readBack != null) {
            for (let i = 0; i < readBack.length; ++i) {
              if (readBack[i] != contentBytes[i]) {
                writeOk = false;
                break;
              }
            }
          }
        } else {
          writeOk = false;
        }
      }
    }
    printed = true;
    const startup = fs.readTextFile("/startup.nsh");
    const readable = startup != null && startup.indexOf("BOOTX64.EFI") >= 0;
    std.println("Hello from AssemblyScript on WASMOS!");
    std.println("This is a tiny WASMOS-APP written in AS.");
    std.printf("Entry: main, runtime: stub\n");
    std.println("startup.nsh readable: " + (readable ? "true" : "false"));
    std.println("long filename write: " + (writeOk ? "true" : "false"));
  }
  return 0;
}
