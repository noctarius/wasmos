import { std } from "./wasmos";

/* TODO(mouse-startup): wire mouse driver into device-manager startup policy
 * once compositor pointer-event routing is implemented end-to-end. */

const CTRL_STATUS_PORT: i32 = 0x64;
const CTRL_CMD_PORT: i32 = 0x64;
const CTRL_DATA_PORT: i32 = 0x60;

const STATUS_OBF: i32 = 0x01;
const STATUS_IBF: i32 = 0x02;
const STATUS_AUX: i32 = 0x20;

const SVC_IPC_REGISTER_REQ: i32 = 0x220;
const SVC_IPC_REGISTER_RESP: i32 = 0x2A0;

const MOUSE_IPC_SUBSCRIBE_REQ: i32 = 0x810;
const MOUSE_IPC_SUBSCRIBE_RESP: i32 = 0x890;
const MOUSE_IPC_MOVE_NOTIFY: i32 = 0x811;

@external("wasmos", "io_in8")
declare function io_in8(port: i32): i32;
@external("wasmos", "io_out8")
declare function io_out8(port: i32, value: i32): i32;
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
@external("wasmos", "irq_unroute")
declare function irq_unroute(irq: i32): i32;
@external("wasmos", "ipc_last_field")
declare function ipc_last_field(field: i32): i32;
@external("wasmos", "ipc_send")
declare function ipc_send(dest: i32, src: i32, type: i32, req_id: i32,
                          arg0: i32, arg1: i32, arg2: i32, arg3: i32): i32;

const MOUSE_IRQ: i32 = 12;
const MOUSE_IPC_IRQ_EVENT: i32 = 0xFF00;

let g_mouse_ep: i32 = -1;
let g_packet_state: i32 = 0;
let g_packet0: i32 = 0;
let g_packet1: i32 = 0;

/* Up to 4 subscriber endpoints; -1 = empty slot. */
let g_subs0: i32 = -1;
let g_subs1: i32 = -1;
let g_subs2: i32 = -1;
let g_subs3: i32 = -1;

function addSubscriber(ep: i32): i32 {
  if (g_subs0 == ep || g_subs1 == ep || g_subs2 == ep || g_subs3 == ep) {
    return 0;
  }
  if (g_subs0 < 0) { g_subs0 = ep; return 0; }
  if (g_subs1 < 0) { g_subs1 = ep; return 0; }
  if (g_subs2 < 0) { g_subs2 = ep; return 0; }
  if (g_subs3 < 0) { g_subs3 = ep; return 0; }
  return -1;
}

function notifySubscribers(dx: i32, dy: i32, buttons: i32): void {
  if (g_subs0 >= 0) {
    ipc_send(g_subs0, g_mouse_ep, MOUSE_IPC_MOVE_NOTIFY, 0, dx, dy, buttons, 0);
  }
  if (g_subs1 >= 0) {
    ipc_send(g_subs1, g_mouse_ep, MOUSE_IPC_MOVE_NOTIFY, 0, dx, dy, buttons, 0);
  }
  if (g_subs2 >= 0) {
    ipc_send(g_subs2, g_mouse_ep, MOUSE_IPC_MOVE_NOTIFY, 0, dx, dy, buttons, 0);
  }
  if (g_subs3 >= 0) {
    ipc_send(g_subs3, g_mouse_ep, MOUSE_IPC_MOVE_NOTIFY, 0, dx, dy, buttons, 0);
  }
}

function flushOutputBuffer(): void {
  for (let i = 0; i < 64; ++i) {
    let st = io_in8(CTRL_STATUS_PORT);
    if ((st & STATUS_OBF) == 0) {
      return;
    }
    let _ = io_in8(CTRL_DATA_PORT);
    io_wait();
  }
}

function waitInputReady(limit: i32 = 100000): bool {
  for (let i = 0; i < limit; ++i) {
    if ((io_in8(CTRL_STATUS_PORT) & STATUS_IBF) == 0) {
      return true;
    }
    if ((i & 0xFF) == 0) {
      sched_yield();
    }
    io_wait();
  }
  return false;
}

function sendControllerCommand(cmd: i32): bool {
  if (!waitInputReady()) {
    return false;
  }
  io_out8(CTRL_CMD_PORT, cmd);
  io_wait();
  return true;
}

function sendMouseCommand(cmd: i32): bool {
  if (!sendControllerCommand(0xD4)) {
    return false;
  }
  if (!waitInputReady()) {
    return false;
  }
  io_out8(CTRL_DATA_PORT, cmd);
  io_wait();
  return true;
}

function readAuxByte(): i32 {
  let st = io_in8(CTRL_STATUS_PORT);
  if ((st & STATUS_OBF) == 0) {
    return -1;
  }
  if ((st & STATUS_AUX) == 0) {
    /* Not our byte: leave it for keyboard driver. */
    return -2;
  }
  return io_in8(CTRL_DATA_PORT) & 0xFF;
}

function readAuxAck(limit: i32 = 50000): i32 {
  for (let i = 0; i < limit; ++i) {
    let v = readAuxByte();
    if (v >= 0) {
      return v;
    }
    if ((i & 0xFF) == 0) {
      sched_yield();
    }
    io_wait();
  }
  return -1;
}

function initMouseDevice(): bool {
  flushOutputBuffer();

  /* Enable AUX port and request streaming in fail-open mode.
   * Some virtual/slow controllers may delay ACKs; mouse support should never
   * block system bootstrap. */
  sendControllerCommand(0xA8);
  sendMouseCommand(0xF6);
  readAuxAck(4096);
  sendMouseCommand(0xF4);
  readAuxAck(4096);
  return true;
}

function drainIpc(): void {
  for (;;) {
    let rc = ipc_try_recv(g_mouse_ep);
    if (rc != 1) {
      break;
    }

    let type = ipc_last_field(0);
    let req_id = ipc_last_field(1);
    let source = ipc_last_field(4);

    if (type == MOUSE_IPC_SUBSCRIBE_REQ) {
      let ok: i32 = (source >= 0) ? addSubscriber(source) : -1;
      if (source >= 0) {
        ipc_send(source, g_mouse_ep, MOUSE_IPC_SUBSCRIBE_RESP, req_id, ok, 0, 0, 0);
      }
    }
  }
}

function handleAuxByte(byte: i32): void {
  if (g_packet_state == 0) {
    /* Packet sync: bit 3 must be set on first byte. */
    if ((byte & 0x08) == 0) {
      return;
    }
    g_packet0 = byte;
    g_packet_state = 1;
    return;
  }
  if (g_packet_state == 1) {
    g_packet1 = byte;
    g_packet_state = 2;
    return;
  }

  g_packet_state = 0;
  let p0 = g_packet0;
  let p1 = g_packet1;
  let p2 = byte;

  if ((p0 & 0xC0) != 0) {
    /* Overflow bits set, drop packet. */
    return;
  }

  let dx: i32 = (p1 << 24) >> 24;
  let dy: i32 = -((p2 << 24) >> 24);
  let buttons: i32 = p0 & 0x07;
  notifySubscribers(dx, dy, buttons);
}

export function initialize(proc_endpoint: i32, _arg1: i32,
                            _arg2: i32, _arg3: i32): i32 {
  g_mouse_ep = ipc_create_endpoint();
  if (g_mouse_ep >= 0) {
    let mouse_name = 0x73756F6D; /* "mous" */
    let req_id = 1;
    if (ipc_send(proc_endpoint, g_mouse_ep, SVC_IPC_REGISTER_REQ, req_id,
                 mouse_name, 0x65, 0, 0) == 0) {
      if (ipc_recv(g_mouse_ep) == 1) {
        if (ipc_last_field(0) != SVC_IPC_REGISTER_RESP || ipc_last_field(1) != req_id || ipc_last_field(2) != 0) {
          g_mouse_ep = -1;
        }
      } else {
        g_mouse_ep = -1;
      }
    } else {
      g_mouse_ep = -1;
    }
  }

  if (g_mouse_ep < 0) {
    std.printf("[mouse] no IPC endpoint, log-only mode\n");
    for (;;) {
      io_wait();
      sched_yield();
    }
    return 0;
  }

  if (!initMouseDevice()) {
    std.printf("[mouse] init failed\n");
  }

  let irq_ok: i32 = irq_route_ipc(MOUSE_IRQ, g_mouse_ep);
  if (irq_ok != 0) {
    std.printf("[mouse] IRQ route failed, falling back to polling\n");
  } else {
    std.printf("[mouse] driver starting (IRQ-driven)\n");
  }

  if (irq_ok != 0) {
    std.printf("[mouse] driver starting (polling)\n");
    for (;;) {
      drainIpc();
      let b = readAuxByte();
      if (b >= 0) {
        handleAuxByte(b);
        io_wait();
      } else {
        sched_yield();
        sched_yield();
      }
      sched_yield();
    }
    return 0;
  }

  for (;;) {
    /* Block until either a client message or an IRQ event arrives. */
    if (ipc_recv(g_mouse_ep) != 1) {
      continue;
    }
    let type: i32 = ipc_last_field(0);
    if (type == MOUSE_IPC_SUBSCRIBE_REQ) {
      let req_id: i32 = ipc_last_field(1);
      let source: i32 = ipc_last_field(4);
      let ok: i32 = (source >= 0) ? addSubscriber(source) : -1;
      if (source >= 0) {
        ipc_send(source, g_mouse_ep, MOUSE_IPC_SUBSCRIBE_RESP, req_id, ok, 0, 0, 0);
      }
    } else if (type == MOUSE_IPC_IRQ_EVENT) {
      let b = readAuxByte();
      if (b >= 0) {
        handleAuxByte(b);
      }
    }
    /* Ignore unknown message types. */
  }

  return 0;
}
