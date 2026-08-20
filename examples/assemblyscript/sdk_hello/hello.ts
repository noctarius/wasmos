// sdk_hello (AssemblyScript) - the AssemblyScript driver's own smoke app.
//
// Plain AssemblyScript against the ./wasmos shim, built by the staged SDK
// (cmake/WasmosSdk.cmake) with no flags but -o and no linker.metadata of its own.
// What it exercises is the part a developer never sees: asc has no include path,
// so the whole AS runtime is staged flat beside a copy of this file, which is
// itself staged as app.ts because the entry is the runtime's runtime.ts importing
// "./app". A `main` signature that omits the args parameter does not compile, and
// that is the shape the driver has to get right.
//
// Deliberately minimal. The AssemblyScript binding's breadth is covered by
// examples/assemblyscript/hello; what is under test here is the driver.
import {std} from "./wasmos";

export function main(args: Array<string>): i32 {
    std.println("Hello WASMOS from AssemblyScript via the SDK!");
    return 0;
}
