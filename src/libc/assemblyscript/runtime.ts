import {runMain} from "./wasmos";
import {main} from "./app";

export function wasmos_main(): i32 {
    return runMain(main);
}
