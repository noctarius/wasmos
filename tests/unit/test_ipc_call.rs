//! `ipc::call` in the Rust binding (src/libc/rust/wasmos.rs), run on the build
//! host against a fake kernel mailbox that stands in for the four IPC host
//! calls it reaches.
//!
//! Regression: 2026-11-26-lang-ipc-call-match -- `call` returned the first
//! message that arrived on the context's managed reply endpoint without
//! checking its request id or source, so a stale reply, or a message with the
//! right id from another endpoint, was handed back as the answer to the
//! request just sent. The C helper (`wasmos_ipc_call`) accepts only a message
//! carrying the request's id AND sent from the endpoint the request went to.
//!
//! The fake server on SERVER answers every request on the caller's reply
//! endpoint with `arg0 + 1`. `arg1` selects what it queues AHEAD of that
//! answer: nothing, a stale reply (another request id), or an impostor (this
//! request id, another source). Every case also checks that `call` consumed the
//! queue, so a message it skipped is not left for the next call.
//!
//! Regression: 2026-11-26-lang-ipc-call-hangup -- `call` never registered
//! `ipc_peer_await` before sending and never recognised `WASMOS_IPC_HANGUP`, so
//! a server that died before answering left the caller blocked forever: the
//! kernel owes a hangup only to an endpoint the server had sent to, or to one
//! that registered an await (docs/architecture/09-process-and-ipc.md, "Peer
//! Hangup"). The fake's `ipc_peer_await` records the registration and refuses
//! DEAD, an endpoint with no owner, with `WASMOS_NOENT`; HANGUP_INSTEAD has the
//! server queue a hangup naming itself in place of an answer.
//!
//! The harness must run single-threaded (`--test-threads=1`): the binding keeps
//! its reply endpoint and request-id counter in statics, as a guest does.
#![allow(dead_code)]

#[path = "../../src/libc/rust/wasmos.rs"]
mod wasmos;

use std::collections::VecDeque;
use std::sync::Mutex;

/// The guest entry point wasmos.rs calls from `wasmos_main`; unused here.
fn main(_args: &[&str]) -> i32 {
    0
}

const SERVER: i32 = 5;
const IMPOSTOR: i32 = 6;
const REPLY_ENDPOINT: i32 = 90;
const SCENARIO_CLEAN: i32 = 0;
const SCENARIO_STALE_FIRST: i32 = 1;
const SCENARIO_IMPOSTOR_FIRST: i32 = 2;
const SCENARIO_HANGUP_INSTEAD: i32 = 3;
const DEAD: i32 = 7;
const WASMOS_NOENT: i32 = -4;
const WASMOS_IPC_HANGUP: i32 = 0xFF03;

#[derive(Clone, Copy, Default)]
struct Message {
    fields: [i32; 8],
}

/// Field indices of `ipc_last_field` (abi/hostcalls.yaml).
const F_TYPE: usize = 0;
const F_REQUEST_ID: usize = 1;
const F_ARG0: usize = 2;
const F_ARG1: usize = 3;
const F_SOURCE: usize = 4;
const F_DESTINATION: usize = 5;

struct Mailbox {
    queue: VecDeque<Message>,
    last: Message,
    /// The endpoint the last `ipc_peer_await` on the reply endpoint named.
    awaited: i32,
    sends: u32,
    sends_unawaited: u32,
}

static MAILBOX: Mutex<Mailbox> = Mutex::new(Mailbox {
    queue: VecDeque::new(),
    last: Message { fields: [0; 8] },
    awaited: -1,
    sends: 0,
    sends_unawaited: 0,
});

#[no_mangle]
pub extern "C" fn ipc_peer_await(waiter: i32, awaited: i32) -> i32 {
    assert_eq!(waiter, REPLY_ENDPOINT);
    if awaited == DEAD {
        return WASMOS_NOENT;
    }
    MAILBOX.lock().unwrap().awaited = awaited;
    0
}

fn message(msg_type: i32, request_id: i32, source: i32, arg0: i32) -> Message {
    let mut m = Message::default();
    m.fields[F_TYPE] = msg_type;
    m.fields[F_REQUEST_ID] = request_id;
    m.fields[F_SOURCE] = source;
    m.fields[F_DESTINATION] = REPLY_ENDPOINT;
    m.fields[F_ARG0] = arg0;
    m
}

#[no_mangle]
pub extern "C" fn ipc_create_endpoint() -> i32 {
    REPLY_ENDPOINT
}

#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub extern "C" fn ipc_send(
    destination: i32,
    source: i32,
    msg_type: i32,
    request_id: i32,
    arg0: i32,
    arg1: i32,
    _arg2: i32,
    _arg3: i32,
) -> i32 {
    assert_eq!(source, REPLY_ENDPOINT);
    let mut mb = MAILBOX.lock().unwrap();
    mb.sends += 1;
    if mb.awaited != destination {
        mb.sends_unawaited += 1;
    }
    if destination != SERVER {
        return 0;
    }
    if arg1 == SCENARIO_HANGUP_INSTEAD {
        let mut hangup = message(WASMOS_IPC_HANGUP, 0, -1, SERVER);
        hangup.fields[F_ARG1] = 42;
        mb.queue.push_back(hangup);
        return 0;
    }
    match arg1 {
        SCENARIO_STALE_FIRST => {
            mb.queue
                .push_back(message(msg_type, request_id + 1000, SERVER, -1))
        }
        SCENARIO_IMPOSTOR_FIRST => mb
            .queue
            .push_back(message(msg_type, request_id, IMPOSTOR, -1)),
        _ => {}
    }
    mb.queue
        .push_back(message(msg_type, request_id, SERVER, arg0 + 1));
    0
}

/// Pops the next message into the "last received" slot; an empty queue is a
/// receive error rather than a block, so a `call` that keeps waiting fails the
/// case instead of hanging it.
#[no_mangle]
pub extern "C" fn ipc_select_one(endpoint: i32) -> i32 {
    assert_eq!(endpoint, REPLY_ENDPOINT);
    let mut mb = MAILBOX.lock().unwrap();
    match mb.queue.pop_front() {
        Some(m) => {
            mb.last = m;
            0
        }
        None => -1,
    }
}

#[no_mangle]
pub extern "C" fn ipc_last_field(field: i32) -> i32 {
    MAILBOX.lock().unwrap().last.fields[field as usize]
}

fn reset() {
    let mut mb = MAILBOX.lock().unwrap();
    mb.queue.clear();
    mb.last = Message::default();
    mb.awaited = -1;
    mb.sends = 0;
    mb.sends_unawaited = 0;
}

fn queue_len() -> usize {
    MAILBOX.lock().unwrap().queue.len()
}

fn call(arg0: i32, scenario: i32) -> wasmos::ipc::Reply {
    match wasmos::ipc::call(SERVER, 0x10, arg0, scenario, 0, 0) {
        Ok(reply) => reply,
        Err(_) => panic!("call failed: the answer never matched"),
    }
}

#[test]
fn a_clean_reply_is_returned() {
    reset();
    let reply = call(41, SCENARIO_CLEAN);
    assert_eq!(reply.arg0, 42);
    assert_eq!(reply.source, SERVER);
    assert_eq!(queue_len(), 0);
}

#[test]
fn a_stale_reply_queued_ahead_is_skipped() {
    reset();
    let reply = call(10, SCENARIO_STALE_FIRST);
    assert_eq!(reply.arg0, 11, "the stale reply was returned as the answer");
    assert_eq!(queue_len(), 0);
}

#[test]
fn a_reply_from_another_endpoint_is_skipped() {
    reset();
    let reply = call(20, SCENARIO_IMPOSTOR_FIRST);
    assert_eq!(
        reply.arg0, 21,
        "a message from another endpoint was returned"
    );
    assert_eq!(reply.source, SERVER);
    assert_eq!(queue_len(), 0);
}

#[test]
fn consecutive_calls_each_get_their_own_reply() {
    reset();
    let first = call(1, SCENARIO_STALE_FIRST);
    let second = call(2, SCENARIO_IMPOSTOR_FIRST);
    assert_eq!(first.arg0, 2);
    assert_eq!(second.arg0, 3);
    assert_ne!(first.request_id, second.request_id);
    assert_eq!(queue_len(), 0);
}

#[test]
fn the_server_is_awaited_before_the_request_goes_out() {
    reset();
    let _ = call(1, SCENARIO_CLEAN);
    let mb = MAILBOX.lock().unwrap();
    assert_eq!(mb.sends, 1);
    assert_eq!(
        mb.sends_unawaited, 0,
        "the request went out before an await"
    );
}

#[test]
fn a_hangup_naming_the_server_ends_the_call() {
    reset();
    let result = wasmos::ipc::call(SERVER, 0x10, 1, SCENARIO_HANGUP_INSTEAD, 0, 0);
    assert!(
        matches!(result, Err(wasmos::Error::PeerGone)),
        "the server's hangup was not reported as PeerGone"
    );
    assert_eq!(queue_len(), 0);
}

#[test]
fn a_server_with_no_owner_is_refused_before_sending() {
    reset();
    let result = wasmos::ipc::call(DEAD, 0x10, 1, SCENARIO_CLEAN, 0, 0);
    assert!(
        matches!(result, Err(wasmos::Error::PeerGone)),
        "a call to an endpoint with no owner was not refused as PeerGone"
    );
    assert_eq!(
        MAILBOX.lock().unwrap().sends,
        0,
        "the request was sent anyway"
    );
}
