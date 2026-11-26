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
}

static MAILBOX: Mutex<Mailbox> = Mutex::new(Mailbox {
    queue: VecDeque::new(),
    last: Message { fields: [0; 8] },
});

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
    assert_eq!(destination, SERVER);
    assert_eq!(source, REPLY_ENDPOINT);
    let mut mb = MAILBOX.lock().unwrap();
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
