/* Host harness for AssemblyScript unit tests.
 *
 * asc compiles the test to wasm; node instantiates it and calls the exported
 * runTests(), which returns 0 or a marker identifying the failed assertion.
 * `--runtime stub` modules have no GC and no WASI, so the only import the
 * coroutine test needs is abort.
 *
 * The `wasmos` import object below is a fake IPC fabric for tests that exercise
 * the event loop: per-endpoint message queues plus a last-received slot, which
 * is exactly the shape the real hostcalls present to a guest. It is inert for
 * tests that never call it.
 *
 * The `harness` module is the control surface the AS test drives it through --
 * plant a message, script a peer reply, make the next send fail, count blocking
 * waits. Keeping the script in the AS test rather than here means one dumb
 * fabric serves every case and the expectations live next to the assertions.
 */
import { readFileSync } from "node:fs";

const [wasmPath, label] = process.argv.slice(2);

/* Randomized case order, replayable via WASMOS_TEST_SEED. The guest exports
   only runTests(), so the case list and the shuffle live there
   (tests/unit/as/shuffle.ts); this side supplies the seed and prints what
   failed. */
const configuredSeed = process.env.WASMOS_TEST_SEED;
const testSeed = configuredSeed
  ? BigInt.asIntN(64, BigInt(configuredSeed))
  : BigInt.asIntN(64, BigInt(Date.now()) * 0x2545f4914f6cdd1dn);
const failedOrder = [];

/* endpoint -> queued messages, in arrival order. */
const queues = new Map();
/* The caller's last-received message, as ipc_last_field reads it. */
let last = new Array(8).fill(0);
let nextSelectId = 1;
const selects = new Map();

/* Test-controlled fabric behaviour. */
/* Destination whose sends fail; -1 disables it. Sends to any negative
   destination fail regardless, since an endpoint handle is never negative. */
let sendFailFrom = -1;
let waitCount = 0;
let sendCount = 0;
let selectBroken = false;
/* One scripted delivery performed the next time the guest blocks (either wait
 * variant), modelling a peer that replies while the guest is blocked. */
let onWait = null;
/* Timeouts the guest asked for, so a test can assert it blocked rather than
   spun, and for how long. */
let timeoutWaits = [];

const FIELD_ORDER = ["type", "requestId", "arg0", "arg1", "source", "destination", "arg2", "arg3"];

function queueFor(endpoint) {
  let q = queues.get(endpoint);
  if (!q) {
    q = [];
    queues.set(endpoint, q);
  }
  return q;
}

function plant(endpoint, type, requestId, source, arg0, arg1, arg2, arg3) {
  queueFor(endpoint).push({
    type,
    requestId,
    source,
    destination: endpoint,
    arg0,
    arg1,
    arg2,
    arg3,
  });
}

const wasmos = {
  ipc_send: (dest, src, type, reqId, a0, a1, a2, a3) => {
    sendCount++;
    if (dest === sendFailFrom || dest < 0) return -1;
    plant(dest, type, reqId, src, a0, a1, a2, a3);
    return 0;
  },
  ipc_drain: (endpoint) => {
    const q = queues.get(endpoint);
    if (!q || q.length === 0) return 0;
    const msg = q.shift();
    last = FIELD_ORDER.map((name) => msg[name]);
    return 1;
  },
  ipc_last_field: (field) => (field >= 0 && field < last.length ? last[field] : 0),
  ipc_select_create: () => {
    if (selectBroken) return -1;
    const id = nextSelectId++;
    selects.set(id, []);
    return id;
  },
  ipc_select_add: (selectId, endpoint) => {
    const set = selects.get(selectId);
    if (!set) return -1;
    set.push(endpoint);
    return 0;
  },
  ipc_select_wait: (selectId) => {
    waitCount++;
    /* A real wait returns when a peer sends. Single-threaded here, so the test
     * scripts that peer's send as the thing the wait unblocks on. The first
     * endpoint in the set is reported ready either way; the guest re-drains
     * afterwards, so a wait that delivered nothing reads as a spurious wake. */
    if (onWait) {
      const deliver = onWait;
      onWait = null;
      deliver();
    }
    const set = selects.get(selectId);
    return set && set.length > 0 ? set[0] : -1;
  },
  ipc_select_wait_timeout: (selectId, timeoutMs) => {
    timeoutWaits.push(timeoutMs);
    return wasmos.ipc_select_wait(selectId);
  },
  ipc_select_destroy: (selectId) => {
    selects.delete(selectId);
    return 0;
  },
};

const harness = {
  /* Queue a message as if a peer had sent it to `endpoint`. */
  plant: (endpoint, type, requestId, source, a0, a1, a2, a3) => {
    plant(endpoint, type, requestId, source, a0, a1, a2, a3);
    return 0;
  },
  /* Deliver this message the next time the guest blocks, not before. */
  plantOnWait: (endpoint, type, requestId, source, a0, a1, a2, a3) => {
    onWait = () => plant(endpoint, type, requestId, source, a0, a1, a2, a3);
    return 0;
  },
  /* Sends addressed to `endpoint` fail; -1 disables. */
  failSendsTo: (endpoint) => {
    sendFailFrom = endpoint;
    return 0;
  },
  pending: (endpoint) => {
    const q = queues.get(endpoint);
    return q ? q.length : 0;
  },
  /* Read a field of a queued message without consuming it. */
  peek: (endpoint, index, field) => {
    const q = queues.get(endpoint);
    if (!q || index >= q.length) return 0;
    const name = FIELD_ORDER[field];
    return name === undefined ? 0 : q[index][name];
  },
  /* Seed for the randomized case order, printed as the guest takes it rather
     than on failure: a suite that traps or hangs never reaches its failure
     path, and that is the run whose order you need back. */
  seed: () => {
    console.log(`${label}: WASMOS_TEST_SEED=0x${BigInt.asUintN(64, testSeed).toString(16)}`);
    return testSeed;
  },
  reportSeed: (seed) => {
    console.log(`${label}: replay this order with WASMOS_TEST_SEED=0x${BigInt.asUintN(64, seed).toString(16)}`);
  },
  reportOrder: (index) => {
    failedOrder.push(index);
  },
  waitCount: () => waitCount,
  timeoutWaitCount: () => timeoutWaits.length,
  lastTimeoutMs: () => (timeoutWaits.length ? timeoutWaits[timeoutWaits.length - 1] : -1),
  /* Refuse select-set creation, so a test can drive the cannot-block path. */
  breakSelect: (broken) => {
    selectBroken = broken !== 0;
    return 0;
  },
  sendCount: () => sendCount,
  reset: () => {
    queues.clear();
    selects.clear();
    last = new Array(8).fill(0);
    nextSelectId = 1;
    sendFailFrom = -1;
    waitCount = 0;
    sendCount = 0;
    onWait = null;
    timeoutWaits = [];
    selectBroken = false;
    return 0;
  },
};

const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), {
  env: {
    abort: (_msg, _file, line, col) => {
      throw new Error(`${label}: abort at ${line}:${col}`);
    },
  },
  wasmos,
  harness,
});

const rc = instance.exports.runTests();
if (rc === 0) {
  console.log(`${label}: ok`);
  process.exit(0);
}
console.log(`${label}: FAIL, assertion marker ${rc}`);
if (failedOrder.length) {
  console.log(`${label}: case order was ${failedOrder.join(", ")}`);
}
process.exit(1);
