import { std } from "./wasmos";

@external("wasmos", "sched_cpu_count")
declare function sched_cpu_count(): i32;

@external("wasmos", "sched_cpu_stats")
declare function sched_cpu_stats(cpu_id: i32, out_ptr: i32): i32;

function padLeft(s: string, width: i32): string {
  while (s.length < width) s = " " + s;
  return s;
}

export function main(_args: Array<string>): i32 {
  const ncpus = sched_cpu_count();
  if (ncpus <= 0) {
    std.println("sched_info: no CPUs reported");
    return 1;
  }

  std.println(" cpu  ready  running(pid)  last(pid)  steals  dispatched");

  const buf = new Uint32Array(5);

  for (let c: i32 = 0; c < ncpus; ++c) {
    if (sched_cpu_stats(c, buf.dataStart as i32) != 0) {
      continue;
    }
    const ready      = buf[0] as i32;
    const running    = buf[1] as i32;
    const steals     = buf[2] as i32;
    const dispatched = buf[3] as i32;
    const last       = buf[4] as i32;
    const row = padLeft(c.toString(), 4) + "  " +
                padLeft(ready.toString(), 5) + "  " +
                padLeft(running.toString(), 11) + "  " +
                padLeft(last.toString(), 9) + "  " +
                padLeft(steals.toString(), 6) + "  " +
                padLeft(dispatched.toString(), 10);
    std.println(row);
  }

  return 0;
}
