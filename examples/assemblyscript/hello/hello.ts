export const WASMOS_WASM_STEP_YIELDED: i32 = 0;

// Minimal AssemblyScript WASMOS-APP entry point.
export function hello_step(
  _type: i32,
  _arg0: i32,
  _arg1: i32,
  _arg2: i32,
  _arg3: i32
): i32 {
  return WASMOS_WASM_STEP_YIELDED;
}
