/* Host harness for the Go (TinyGo) coroutine test.
 *
 * Unlike the Rust and Zig suites, which run on the host against the C
 * implementation, the Go binding's records are opaque blobs sized for wasm32 --
 * so it has to be exercised on that target. TinyGo builds the module against
 * the same wasmos-tinygo.json the Go examples use, which already compiles
 * coroutine_wasm.c into it; node instantiates it and calls runTests(), which
 * returns 0 or a marker identifying the failed assertion.
 *
 * The module imports only wasmos.proc_exit, stubbed here. */
import { readFileSync } from "node:fs";

const [wasmPath, label] = process.argv.slice(2);
let exited = null;

const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), {
  wasmos: {
    proc_exit: (code) => {
      exited = code;
      throw new Error(`${label}: module called proc_exit(${code})`);
    },
  },
});

/* TinyGo initialises package globals in _initialize; calling an export before
 * it runs leaves them zero, which traps on the first map/slice access. */
if (typeof instance.exports._initialize === "function") {
  instance.exports._initialize();
}

const rc = instance.exports.runTests();
if (rc === 0 && exited === null) {
  console.log(`${label}: ok`);
  process.exit(0);
}
console.log(`${label}: FAIL, assertion marker ${rc}`);
process.exit(1);
