/* Host harness for the Go (TinyGo) coroutine test.
 *
 * Unlike the Rust and Zig suites, which run on the host against the C
 * implementation, the Go binding's records are opaque blobs sized for wasm32 --
 * so it has to be exercised on that target. TinyGo builds the module against
 * the same wasmos-tinygo.json the Go examples use, which already compiles
 * coroutine_wasm.c into it; node instantiates it and calls runTests(), which
 * returns 0 or a marker identifying the failed assertion.
 *
 * The module is linked against the whole Go binding, so every host call
 * wasmos.go declares is an import whether or not the coroutine tests reach it.
 * An unstubbed one is a LinkError at instantiate, not a test failure, so adding
 * a //go:wasmimport to wasmos.go means adding it here too. */
import { readFileSync } from "node:fs";

const [wasmPath, label] = process.argv.slice(2);
let exited = null;

const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), {
  wasmos: {
    proc_exit: (code) => {
      exited = code;
      throw new Error(`${label}: module called proc_exit(${code})`);
    },
    /* No process manager here, so there is no spawn-info buffer: 0 is the
       documented "none" answer, and the Go port's loadSpawnInfo treats it as
       "leave the record zeroed". That short-circuit is also why xfer_buffer_read
       is never actually called -- it is stubbed because an import must resolve
       at instantiate, not because the tests reach it. */
    spawn_info_buffer: () => 0,
    xfer_buffer_read: () => -1,
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
