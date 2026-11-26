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

/* A fake kernel mailbox for ipc.Call (testIPCCallMatchesItsReply). A request to
 * SERVER is answered on the sender's endpoint with arg0 + 1; arg1 selects what
 * is queued AHEAD of that answer: nothing (0), a stale reply carrying another
 * request id (1), or an impostor carrying this request id from another endpoint
 * (2). Both decoys carry arg0 = -1. ipc_select_one pops into the
 * "last received" slot and fails on an empty queue rather than blocking, so a
 * Call that keeps waiting fails its case instead of hanging the suite.
 * ipc_pending (module wasmos_test) reports what is still queued.
 *
 * ipc_peer_await records the endpoint the reply endpoint awaits and refuses
 * DEAD, an endpoint with no owner, with WASMOS_NOENT; arg1 = 3 has the server
 * queue a WASMOS_IPC_HANGUP naming itself in place of an answer.
 * ipc_call_stats (module wasmos_test) reports, packed, how many requests went
 * out and how many of those went out before an await on their destination. */
const SERVER = 5;
const IMPOSTOR = 6;
const DEAD = 7;
const CALL_REPLY_ENDPOINT = 90;
const WASMOS_NOENT = -4;
const WASMOS_IPC_HANGUP = 0xff03;
function fakeMailbox() {
  const queues = new Map();
  let last = new Int32Array(8);
  let awaited = -1;
  let sends = 0;
  let sendsUnawaited = 0;
  const push = (endpoint, type, requestId, source, arg0) => {
    const m = new Int32Array(8);
    m[0] = type;
    m[1] = requestId;
    m[2] = arg0;
    m[4] = source;
    m[5] = endpoint;
    if (!queues.has(endpoint)) queues.set(endpoint, []);
    queues.get(endpoint).push(m);
  };
  return {
    ipc_create_endpoint: () => CALL_REPLY_ENDPOINT,
    ipc_peer_await: (waiter, endpoint) => {
      if (endpoint === DEAD) return WASMOS_NOENT;
      if (waiter === CALL_REPLY_ENDPOINT) awaited = endpoint;
      return 0;
    },
    ipc_call_stats: () => {
      const stats = sends * 1000 + sendsUnawaited;
      awaited = -1;
      sends = 0;
      sendsUnawaited = 0;
      return stats;
    },
    ipc_send: (dest, src, type, requestId, arg0, arg1) => {
      if (src === CALL_REPLY_ENDPOINT) {
        sends++;
        if (awaited !== dest) sendsUnawaited++;
      }
      if (dest !== SERVER) return 0;
      if (arg1 === 3) {
        push(src, WASMOS_IPC_HANGUP, 0, -1, SERVER);
        return 0;
      }
      if (arg1 === 1) push(src, type, requestId + 1000, SERVER, -1);
      if (arg1 === 2) push(src, type, requestId, IMPOSTOR, -1);
      push(src, type, requestId, SERVER, arg0 + 1);
      return 0;
    },
    ipc_select_one: (endpoint) => {
      const q = queues.get(endpoint);
      if (!q || q.length === 0) return -1;
      last = q.shift();
      return 0;
    },
    ipc_last_field: (field) => last[field],
    ipc_pending: (endpoint) => (queues.get(endpoint) || []).length,
  };
}
const mailbox = fakeMailbox();
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
    /* The event loop the IPCFuture case drives builds a select set over its
       receiver endpoint at init. No case polls the loop, so the set is never
       waited on: these only have to succeed. */
    ipc_select_create: () => 1,
    ipc_select_add: () => 0,
    ipc_select_destroy: () => 0,
    ipc_create_endpoint: mailbox.ipc_create_endpoint,
    ipc_send: mailbox.ipc_send,
    ipc_select_one: mailbox.ipc_select_one,
    ipc_last_field: mailbox.ipc_last_field,
    ipc_peer_await: mailbox.ipc_peer_await,
  },
  wasmos_test: {
    ipc_pending: mailbox.ipc_pending,
    ipc_call_stats: mailbox.ipc_call_stats,
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
