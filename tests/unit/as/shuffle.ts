/* shuffle.ts - randomized case order for AssemblyScript suites.
 *
 * The AS counterpart of tests/unit/test_shuffle.h, and there for the same
 * reason: a suite written as a fixed chain only proves its cases pass IN THAT
 * ORDER, so a case that leaks state can make its neighbour pass for the wrong
 * reason and nothing notices.
 *
 * The seed comes from the host harness (there is no clock here) and is reported
 * back on failure, so a failing order replays with WASMOS_TEST_SEED. The
 * generator is splitmix64, as in the C helper, but chains its state through the
 * mixed output rather than through the raw counter: a seed reproduces an order
 * within this suite, not the C suite's order for the same seed.
 *
 * Harness imports. All three bind to the `harness` module of run_as_test.mjs and
 * exist only under that runner; nothing in the kernel provides such a module, so
 * a wasm module importing them cannot be instantiated on target. They are
 * documented here rather than on their declarations because the AssemblyScript
 * prettier plugin respaces a decorated declaration once a comment attaches to
 * it.
 *   seed          The seed for this run: WASMOS_TEST_SEED when set, otherwise
 *                 one derived from the host's wall clock. Signed because i64 is
 *                 what crosses the wasm boundary; the value is a bit pattern and
 *                 is reinterpreted as u64 here. Calling it prints the seed, so
 *                 it is called once per run and before the first case -- a suite
 *                 that traps or hangs still gets its seed out.
 *   reportSeed    Print the seed again next to the failure, so it is not only at
 *                 the top of the log.
 *   reportOrder   Append one case index to the order the host prints after a
 *                 failing run. Called once per case actually run, in the order
 *                 taken, so the host list is the prefix that produced the
 *                 failure rather than the whole table.
 */

@external("harness", "seed") declare function harnessSeed(): i64;


@external("harness", "reportSeed") declare function harnessReportSeed(seed: i64): void;


@external("harness", "reportOrder") declare function harnessReportOrder(index: i32): void;

/** A case returns 0 to pass, or a marker identifying its failed assertion. */
export type TestCase = () => i32;

/* One splitmix64 draw from `state`. The caller chains by assigning the result
 * back, so the state advances through the mixed output; the C helper in
 * tests/unit/test_shuffle.h advances a raw counter instead. Same generator, two
 * different sequences from one seed. */
function nextRandom(state: u64): u64 {
    let z: u64 = state + 0x9e3779b97f4a7c15;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
}

/**
 * Run every case in a seed-determined order, stopping at the first failure and
 * returning its marker. Reports the seed and the order taken so the failure can
 * be replayed.
 *
 * Returns 0 when every case passes. `cases` is borrowed and not modified. An
 * empty table returns 0 without running anything, and still draws the seed.
 * Cases after the first failure are not run, so one run reports one failure.
 */
export function runShuffled(cases: StaticArray<TestCase>): i32 {
    const count = cases.length;
    const order = new StaticArray<i32>(count);
    for (let i = 0; i < count; ++i) unchecked((order[i] = i));

    const seed = <u64>harnessSeed();
    let state = seed;
    /* Fisher-Yates, so every permutation is reachable. */
    for (let i = count - 1; i > 0; --i) {
        state = nextRandom(state);
        const j = <i32>(state % <u64>(i + 1));
        const swap = unchecked(order[i]);
        unchecked((order[i] = unchecked(order[j])));
        unchecked((order[j] = swap));
    }

    for (let i = 0; i < count; ++i) {
        const index = unchecked(order[i]);
        const rc = unchecked(cases[index])();
        if (rc != 0) {
            harnessReportSeed(<i64>seed);
            for (let j = 0; j <= i; ++j) harnessReportOrder(unchecked(order[j]));
            return rc;
        }
    }
    return 0;
}
