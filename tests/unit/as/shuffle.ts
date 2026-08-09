/* shuffle.ts - randomized case order for AssemblyScript suites.
 *
 * The AS counterpart of tests/unit/test_shuffle.h, and there for the same
 * reason: a suite written as a fixed chain only proves its cases pass IN THAT
 * ORDER, so a case that leaks state can make its neighbour pass for the wrong
 * reason and nothing notices.
 *
 * The seed comes from the host harness (there is no clock here) and is reported
 * back on failure, so a failing order replays with WASMOS_TEST_SEED. The
 * generator is the same splitmix64 the C helper uses, so a seed means the same
 * order in both.
 */

@external("harness", "seed") declare function harnessSeed(): i64;
@external("harness", "reportSeed") declare function harnessReportSeed(seed: i64): void;
@external("harness", "reportOrder") declare function harnessReportOrder(index: i32): void;

/** A case returns 0 to pass, or a marker identifying its failed assertion. */
export type TestCase = () => i32;

function nextRandom(state: u64): u64 {
    let z: u64 = state + 0x9E3779B97F4A7C15;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EB;
    return z ^ (z >> 31);
}

/**
 * Run every case in a seed-determined order, stopping at the first failure and
 * returning its marker. Reports the seed and the order taken so the failure can
 * be replayed.
 */
export function runShuffled(cases: StaticArray<TestCase>): i32 {
    const count = cases.length;
    const order = new StaticArray<i32>(count);
    for (let i = 0; i < count; ++i)
        unchecked(order[i] = i);

    const seed = <u64>harnessSeed();
    let state = seed;
    /* Fisher-Yates, so every permutation is reachable. */
    for (let i = count - 1; i > 0; --i) {
        state = nextRandom(state);
        const j = <i32>(state % <u64>(i + 1));
        const swap = unchecked(order[i]);
        unchecked(order[i] = unchecked(order[j]));
        unchecked(order[j] = swap);
    }

    for (let i = 0; i < count; ++i) {
        const index = unchecked(order[i]);
        const rc = unchecked(cases[index])();
        if (rc != 0) {
            harnessReportSeed(<i64>seed);
            for (let j = 0; j <= i; ++j)
                harnessReportOrder(unchecked(order[j]));
            return rc;
        }
    }
    return 0;
}
