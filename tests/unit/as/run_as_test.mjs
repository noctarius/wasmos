/* Host harness for AssemblyScript unit tests.
 *
 * asc compiles the test to wasm; node instantiates it and calls the exported
 * runTests(), which returns 0 or a marker identifying the failed assertion.
 * `--runtime stub` modules have no GC and no WASI, so the only import needed is
 * abort. */
import { readFileSync } from "node:fs";

const [wasmPath, label] = process.argv.slice(2);
const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), {
  env: {
    abort: (_msg, _file, line, col) => {
      throw new Error(`${label}: abort at ${line}:${col}`);
    },
  },
});

const rc = instance.exports.runTests();
if (rc === 0) {
  console.log(`${label}: ok`);
  process.exit(0);
}
console.log(`${label}: FAIL, assertion marker ${rc}`);
process.exit(1);
