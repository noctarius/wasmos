const COM1_PORT: i32 = 0x3F8;
const COM1_STATUS: i32 = COM1_PORT + 5;

const IPC_FIELD_TYPE: i32 = 0;
const IPC_FIELD_REQUEST_ID: i32 = 1;
const IPC_FIELD_ARG0: i32 = 2;
const IPC_FIELD_ARG1: i32 = 3;
const IPC_FIELD_SOURCE: i32 = 4;

const SERIAL_DRIVER_WRITE_REQ: i32 = 0x500;
const SERIAL_DRIVER_READ_REQ: i32 = 0x501;
const SERIAL_DRIVER_RESP: i32 = 0x580;
const SERIAL_DRIVER_ERROR: i32 = 0x5FF;

const SERIAL_READ_STATUS_CHAR: i32 = 1;
const SERIAL_READ_STATUS_EMPTY: i32 = 0;
const SERIAL_READ_STATUS_ERROR: i32 = -1;

@external("wasmos", "console_write")
declare function console_write(ptr: i32, len: i32): i32;
@external("wasmos", "ipc_create_endpoint")
declare function ipc_create_endpoint(): i32;
@external("wasmos", "ipc_recv")
declare function ipc_recv(endpoint: i32): i32;
@external("wasmos", "ipc_send")
declare function ipc_send(destination: i32, source: i32, type: i32, request_id: i32, arg0: i32, arg1: i32, arg2: i32, arg3: i32): i32;
@external("wasmos", "ipc_last_field")
declare function ipc_last_field(field: i32): i32;
@external("wasmos", "serial_register")
declare function serial_register(endpoint: i32): i32;
@external("wasmos", "io_in8")
declare function io_in8(port: i32): i32;
@external("wasmos", "io_out8")
declare function io_out8(port: i32, value: i32): i32;
@external("wasmos", "io_wait")
declare function io_wait(): i32;

let g_endpoint: i32 = -1;

function writeString(text: string): void {
  if (text.length == 0) {
    return;
  }
  let bytes = Uint8Array.wrap(String.UTF8.encode(text, false));
  console_write(bytes.dataStart as i32, bytes.byteLength as i32);
}

function serial_init_hw(): void {
  io_out8(COM1_PORT + 1, 0x00);
  io_out8(COM1_PORT + 3, 0x80);
  io_out8(COM1_PORT + 0, 0x01);
  io_out8(COM1_PORT + 1, 0x00);
  io_out8(COM1_PORT + 3, 0x03);
  io_out8(COM1_PORT + 2, 0xC7);
  io_out8(COM1_PORT + 4, 0x0B);
}

function tx_ready(): bool {
  return (io_in8(COM1_STATUS) & 0x20) != 0;
}

function rx_ready(): bool {
  return (io_in8(COM1_STATUS) & 0x01) != 0;
}

function write_port(value: i32): void {
  while (!tx_ready()) {
    io_wait();
  }
  io_out8(COM1_PORT, value & 0xFF);
}

function read_port(): i32 {
  if (!rx_ready()) {
    return -1;
  }
  return io_in8(COM1_PORT) & 0xFF;
}

function send_response(destination: i32, request_id: i32, value: i32, status: i32): void {
  if (destination < 0) {
    return;
  }
  ipc_send(destination, g_endpoint, SERIAL_DRIVER_RESP, request_id, value, status, 0, 0);
}

function handle_write(request_id: i32, source: i32): void {
  let value = ipc_last_field(IPC_FIELD_ARG0);
  write_port(value);
  send_response(source, request_id, 0, 0);
}

function handle_read(request_id: i32, source: i32): void {
  let char_code = read_port();
  if (char_code >= 0) {
    send_response(source, request_id, char_code, SERIAL_READ_STATUS_CHAR);
    return;
  }
  send_response(source, request_id, 0, SERIAL_READ_STATUS_EMPTY);
}

function handle_message(): void {
  if (ipc_recv(g_endpoint) < 0) {
    return;
  }

  let kind = ipc_last_field(IPC_FIELD_TYPE);
  let request_id = ipc_last_field(IPC_FIELD_REQUEST_ID);
  let source = ipc_last_field(IPC_FIELD_SOURCE);
  switch (kind) {
    case SERIAL_DRIVER_WRITE_REQ:
      handle_write(request_id, source);
      break;
    case SERIAL_DRIVER_READ_REQ:
      handle_read(request_id, source);
      break;
    default:
      send_response(source, request_id, 0, SERIAL_READ_STATUS_ERROR);
      break;
  }
}

export function initialize(_proc_endpoint: i32, _module_count: i32, _arg2: i32, _arg3: i32): i32 {

  g_endpoint = ipc_create_endpoint();
  if (g_endpoint < 0) {
    writeString("[serial] endpoint failure\n");
    return -1;
  }
  if (serial_register(g_endpoint) != 0) {
    writeString("[serial] register failure\n");
    return -1;
  }

  serial_init_hw();
  writeString("[serial] driver ready\n");

  for (;;) {
    handle_message();
  }

  return 0;
}
