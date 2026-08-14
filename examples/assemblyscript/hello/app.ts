// WASMOS "hello world" for the AssemblyScript guest binding.
//
// Demonstrates console output (std.println / std.printf) and the synchronous
// filesystem API in ./wasmos: fs.create and fs.openRead return a handle object
// whose write/read/close report success, while fs.unlink, fs.stat and
// fs.readTextFile are one-shot helpers. Unlike the Rust, Zig and Go ports there
// is no async chain here — this binding has no coroutine runtime.
//
// It reads /boot/startup.nsh and looks for a known substring, then creates a
// file with a long (non-8.3) name, writes it, reads it back for comparison,
// unlinks it and confirms the stat now fails, printing one true/false line per
// check for the boot test to match on.
//
// The `printed` guard makes a second call a no-op, so a re-entered main cannot
// duplicate the output the test matches on.
//
// Preconditions: /boot/startup.nsh must exist and contain "BOOTX64.EFI", and
// the working directory must be writable — the test file uses a relative path.
import {fs, std} from "./wasmos";

let printed = false;

// App entry point. `args` carries the spawn arguments and is expected to be
// empty; anything else is reported and otherwise ignored. Always returns 0 —
// the printed lines carry the result.
export function main(args: Array<string>): i32 {
    if (args.length != 0) {
        std.println("unexpected args");
    }
    if (!printed) {
        const path = "assemblyscript-long-file-check.txt";
        const content = String.UTF8.encode("assemblyscript shim long filename\n", false);
        const contentBytes = Uint8Array.wrap(content);
        let writeOk = false;
        let unlinkOk = false;
        const out = fs.create(path);
        if (out != null) {
            writeOk = out.write(contentBytes) == content.byteLength && out.close();
            if (writeOk) {
                const verify = fs.openRead(path);
                if (verify != null) {
                    const readBack = verify.read(content.byteLength);
                    writeOk =
                        verify.close() &&
                        readBack != null &&
                        readBack.length == contentBytes.length;
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
                if (writeOk) {
                    unlinkOk = fs.unlink(path) && fs.stat(path) == null;
                }
            }
        }
        printed = true;
        const startup = fs.readTextFile("/boot/startup.nsh");
        const readable = startup != null && startup.indexOf("BOOTX64.EFI") >= 0;
        std.println("Hello from AssemblyScript on WASMOS!");
        std.println("This is a tiny WASMOS-APP written in AS.");
        std.printf("Entry: main, runtime: stub\n");
        std.println("startup.nsh readable: " + (readable ? "true" : "false"));
        std.println("long filename write: " + (writeOk ? "true" : "false"));
        std.println("long filename unlink: " + (unlinkOk ? "true" : "false"));
    }
    return 0;
}
