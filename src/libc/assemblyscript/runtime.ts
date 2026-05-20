import { runMain } from "./wasmos";
import { main } from "./app";

export function wasmos_main(arg0: i32, arg1: i32, arg2: i32, arg3: i32): i32 {
  return runMain(main, arg0, arg1, arg2, arg3);
}
