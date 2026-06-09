import { std } from "./wasmos";

const KEYBOARD_STATUS_PORT: i32 = 0x64;
const KEYBOARD_DATA_PORT: i32 = 0x60;
const KEYBOARD_OBF_FLAG: i32 = 0x01;
const KEYBOARD_AUX_FLAG: i32 = 0x20;

const KBD_IPC_SUBSCRIBE_REQ:  i32 = 0x800;
const KBD_IPC_SUBSCRIBE_RESP: i32 = 0x880;
const KBD_IPC_KEY_NOTIFY:     i32 = 0x801;
const SVC_IPC_REGISTER_REQ:   i32 = 0x220;
const SVC_IPC_REGISTER_RESP:  i32 = 0x2A0;
const PROC_IPC_NOTIFY_READY:  i32 = 0x20C;

@external("wasmos", "io_in8")
declare function io_in8(port: i32): i32;
@external("wasmos", "io_wait")
declare function io_wait(): i32;
@external("wasmos", "sched_yield")
declare function sched_yield(): i32;
@external("wasmos", "ipc_try_recv")
declare function ipc_try_recv(endpoint: i32): i32;
@external("wasmos", "ipc_recv")
declare function ipc_recv(endpoint: i32): i32;
@external("wasmos", "ipc_create_endpoint")
declare function ipc_create_endpoint(): i32;
@external("wasmos", "irq_route_ipc")
declare function irq_route_ipc(irq: i32, endpoint: i32): i32;
@external("wasmos", "irq_ack")
declare function irq_ack(irq: i32): i32;
@external("wasmos", "irq_unroute")
declare function irq_unroute(irq: i32): i32;
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

const KBD_IPC_IRQ_EVENT: i32 = 0xFF00;
const KBD_IRQ: i32 = 1;

let g_kbd_ep: i32 = -1;
let g_extended_pending: i32 = 0;

function readScancode(): i32 {
  let status = io_in8(KEYBOARD_STATUS_PORT);
  if ((status & KEYBOARD_OBF_FLAG) == 0) {
    return -1;
  }
  if ((status & KEYBOARD_AUX_FLAG) != 0) {
    /* AUX (mouse) byte: leave for mouse driver. */
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

function notifySubscribers(scancode: i32, keyup: i32, extended: i32): void {
  if (g_subs0 >= 0) {
    ipc_send(g_subs0, g_kbd_ep, KBD_IPC_KEY_NOTIFY, 0, scancode, keyup, extended, 0);
  }
  if (g_subs1 >= 0) {
    ipc_send(g_subs1, g_kbd_ep, KBD_IPC_KEY_NOTIFY, 0, scancode, keyup, extended, 0);
  }
  if (g_subs2 >= 0) {
    ipc_send(g_subs2, g_kbd_ep, KBD_IPC_KEY_NOTIFY, 0, scancode, keyup, extended, 0);
  }
  if (g_subs3 >= 0) {
    ipc_send(g_subs3, g_kbd_ep, KBD_IPC_KEY_NOTIFY, 0, scancode, keyup, extended, 0);
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

export function initialize(_proc_endpoint: i32, _arg1: i32,
                            _arg2: i32, _arg3: i32): i32 {
  g_kbd_ep = ipc_create_endpoint();
  if (g_kbd_ep >= 0) {
    let kbd_name = 0x0064626B; /* "kbd\0" */
    let req_id = 1;
    if (ipc_send(_proc_endpoint, g_kbd_ep, SVC_IPC_REGISTER_REQ, req_id,
                 kbd_name, 0, 0, 0) == 0) {
      if (ipc_recv(g_kbd_ep) == 1) {
        if (ipc_last_field(0) != SVC_IPC_REGISTER_RESP || ipc_last_field(1) != req_id || ipc_last_field(2) != 0) {
          g_kbd_ep = -1;
        }
      } else {
        g_kbd_ep = -1;
      }
    } else {
      g_kbd_ep = -1;
    }
  }

  if (g_kbd_ep < 0) {
    std.printf("[keyboard] no IPC endpoint, log-only mode\n");
    for (;;) {
      io_wait();
      sched_yield();
    }
    return 0;
  }

  let irq_ok: i32 = irq_route_ipc(KBD_IRQ, g_kbd_ep);
  if (irq_ok != 0) {
    std.printf("[keyboard] IRQ route failed, falling back to polling\n");
  } else {
    std.printf("[keyboard] driver starting (IRQ-driven)\n");
  }
  ipc_send(_proc_endpoint, g_kbd_ep, PROC_IPC_NOTIFY_READY, 0, 0, 0, 0, 0);

  if (irq_ok != 0) {
    /* Polling fallback. */
    std.printf("[keyboard] driver starting (polling)\n");
    for (;;) {
      drainIpc();
      let code = readScancode();
      if (code >= 0) {
        if (code == 0xE0) { g_extended_pending = 1; io_wait(); sched_yield(); continue; }
        let keyup: i32 = (code & 0x80) != 0 ? 1 : 0;
        let sc: i32 = code & 0x7F;
        std.printf("[keyboard] key sc=" + sc.toString() + " keyup=" + keyup.toString() + "\n");
        notifySubscribers(sc, keyup, g_extended_pending);
        g_extended_pending = 0;
      }
      io_wait();
      sched_yield();
    }
    return 0;
  }

  for (;;) {
    /* Block until either a client message or an IRQ event arrives. */
    if (ipc_recv(g_kbd_ep) != 1) {
      continue;
    }
    let type: i32 = ipc_last_field(0);
    if (type == KBD_IPC_SUBSCRIBE_REQ) {
      let req_id: i32 = ipc_last_field(1);
      let source: i32 = ipc_last_field(4);
      let ok: i32 = (source >= 0) ? addSubscriber(source) : -1;
      if (source >= 0) {
        ipc_send(source, g_kbd_ep, KBD_IPC_SUBSCRIBE_RESP, req_id, ok, 0, 0, 0);
      }
    } else if (type == KBD_IPC_IRQ_EVENT) {
      std.printf("[keyboard] irq-event\n");
      let code = readScancode();
      /* Re-enable IRQ after reading the hardware register so the next keypress
       * can fire the interrupt again.  Must come after io_in8 so OBF is clear
       * before unmasking (prevents immediate re-fire on level-triggered IRQ). */
      irq_ack(KBD_IRQ);
      if (code < 0) {
        continue;
      }
      if (code == 0xE0) {
        g_extended_pending = 1;
        continue;
      }
      /* PS/2 Set 1: key-up codes have bit 7 set. */
      let keyup: i32 = (code & 0x80) != 0 ? 1 : 0;
      let sc: i32    = code & 0x7F;
      let ext: i32   = g_extended_pending;
      g_extended_pending = 0;
      std.printf("[keyboard] key sc=" + sc.toString() + " keyup=" + keyup.toString() + "\n");
      notifySubscribers(sc, keyup, ext);
    }
    /* Ignore unknown message types. */
  }

  return 0;
}
