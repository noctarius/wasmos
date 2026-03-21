const KEYBOARD_STATUS_PORT: i32 = 0x64;
const KEYBOARD_DATA_PORT: i32 = 0x60;
const KEYBOARD_OBF_FLAG: i32 = 0x01;

const KBD_IPC_SUBSCRIBE_REQ:  i32 = 0x800;
const KBD_IPC_SUBSCRIBE_RESP: i32 = 0x880;
const KBD_IPC_KEY_NOTIFY:     i32 = 0x801;

@external("wasmos", "console_write")
declare function console_write(ptr: i32, len: i32): i32;
@external("wasmos", "io_in8")
declare function io_in8(port: i32): i32;
@external("wasmos", "io_wait")
declare function io_wait(): i32;
@external("wasmos", "sched_yield")
declare function sched_yield(): i32;
@external("wasmos", "ipc_try_recv")
declare function ipc_try_recv(endpoint: i32): i32;
@external("wasmos", "ipc_last_field")
declare function ipc_last_field(field: i32): i32;
@external("wasmos", "ipc_send")
declare function ipc_send(dest: i32, src: i32, type: i32, req_id: i32,
                          arg0: i32, arg1: i32, arg2: i32, arg3: i32): i32;

/* Up to 4 subscriber endpoints; -1 = empty slot. */
let g_subs0: i32 = -1;
let g_subs1: i32 = -1;
let g_subs2: i32 = -1;
let g_subs3: i32 = -1;

let g_kbd_ep: i32 = -1;
function writeString(text: string): void {
  if (text.length == 0) { return; }
  let bytes = Uint8Array.wrap(String.UTF8.encode(text, false));
  console_write(bytes.dataStart as i32, bytes.byteLength as i32);
}

function readScancode(): i32 {
  if ((io_in8(KEYBOARD_STATUS_PORT) & KEYBOARD_OBF_FLAG) == 0) {
    return -1;
  }
  return io_in8(KEYBOARD_DATA_PORT) & 0xFF;
}

function addSubscriber(ep: i32): i32 {
  /* Ignore duplicate registrations. */
  if (g_subs0 == ep || g_subs1 == ep || g_subs2 == ep || g_subs3 == ep) {
    return 0;
  }
  if (g_subs0 < 0) { g_subs0 = ep; return 0; }
  if (g_subs1 < 0) { g_subs1 = ep; return 0; }
  if (g_subs2 < 0) { g_subs2 = ep; return 0; }
  if (g_subs3 < 0) { g_subs3 = ep; return 0; }
  return -1; /* full */
}

function notifySubscribers(scancode: i32, keyup: i32): void {
  if (g_subs0 >= 0) {
    ipc_send(g_subs0, g_kbd_ep, KBD_IPC_KEY_NOTIFY, 0, scancode, keyup, 0, 0);
  }
  if (g_subs1 >= 0) {
    ipc_send(g_subs1, g_kbd_ep, KBD_IPC_KEY_NOTIFY, 0, scancode, keyup, 0, 0);
  }
  if (g_subs2 >= 0) {
    ipc_send(g_subs2, g_kbd_ep, KBD_IPC_KEY_NOTIFY, 0, scancode, keyup, 0, 0);
  }
  if (g_subs3 >= 0) {
    ipc_send(g_subs3, g_kbd_ep, KBD_IPC_KEY_NOTIFY, 0, scancode, keyup, 0, 0);
  }
}

/* Drain all pending IPC messages without blocking.
 * ipc_try_recv() returns 1 with message in slot, 0 if empty, -1 on error. */
function drainIpc(): void {
  for (;;) {
    let rc = ipc_try_recv(g_kbd_ep);
    if (rc != 1) { break; }

    let type   = ipc_last_field(0);
    let req_id = ipc_last_field(1);
    let source = ipc_last_field(4);

    if (type == KBD_IPC_SUBSCRIBE_REQ) {
      let ok: i32 = (source >= 0) ? addSubscriber(source) : -1;
      if (source >= 0) {
        ipc_send(source, g_kbd_ep, KBD_IPC_SUBSCRIBE_RESP, req_id, ok, 0, 0, 0);
      }
    }
    /* Ignore unknown message types. */
  }
}

export function initialize(_proc_endpoint: i32, kbd_endpoint: i32,
                            _arg2: i32, _arg3: i32): i32 {
  g_kbd_ep = kbd_endpoint;

  if (g_kbd_ep < 0) {
    writeString("[keyboard] no IPC endpoint, log-only mode\n");
    for (;;) {
      readScancode();
      io_wait();
      sched_yield();
    }
    return 0;
  }

  writeString("[keyboard] driver starting\n");

  for (;;) {
    /* 1. Process any pending subscribe requests. */
    drainIpc();

    /* 2. Check for a new PS/2 scancode. */
    let code = readScancode();
    if (code >= 0) {
      /* PS/2 Set 1: key-up codes have bit 7 set. */
      let keyup: i32 = (code & 0x80) != 0 ? 1 : 0;
      let sc: i32    = code & 0x7F;
      notifySubscribers(sc, keyup);
    }

    io_wait();
    sched_yield();
  }

  return 0;
}
