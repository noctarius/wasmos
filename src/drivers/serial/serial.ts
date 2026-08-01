import {std, startup} from "./wasmos";

const COM1_PORT: i32 = 0x3F8;
const COM1_IER: i32 = COM1_PORT + 1;
const COM1_STATUS: i32 = COM1_PORT + 5;

const IPC_FIELD_TYPE: i32 = 0;
const IPC_FIELD_REQUEST_ID: i32 = 1;
const IPC_FIELD_ARG0: i32 = 2;
const IPC_FIELD_ARG1: i32 = 3;
const IPC_FIELD_SOURCE: i32 = 4;

const SERIAL_DRIVER_WRITE_REQ: i32 = 0x500;
const SERIAL_DRIVER_READ_REQ: i32 = 0x501;
const SERIAL_IPC_SUBSCRIBE_REQ: i32 = 0x502;
const SERIAL_IPC_SUBSCRIBE_RESP: i32 = 0x582;
const SERIAL_DRIVER_RESP: i32 = 0x580;
const SERIAL_DRIVER_ERROR: i32 = 0x5FF;
const PROC_IPC_NOTIFY_READY: i32 = 0x20C;
const SVC_IPC_REGISTER_REQ: i32 = 0x220;
const SVC_IPC_REGISTER_RESP: i32 = 0x2A0;

/* vt-facing push: RX bytes for the serial-bound slot, packed like
 * VT_IPC_WRITE_REQ (arg0[27:24]=byte_count, arg0[7:0]=byte). */
const VT_IPC_SERIAL_INPUT_REQ: i32 = 0x707;

const SERIAL_IRQ: i32 = 4;
const SERIAL_IPC_IRQ_EVENT: i32 = 0xFF00;

const SERIAL_READ_STATUS_CHAR: i32 = 1;
const SERIAL_READ_STATUS_EMPTY: i32 = 0;
const SERIAL_READ_STATUS_ERROR: i32 = -1;

@external("wasmos", "ipc_create_endpoint") declare function ipc_create_endpoint(): i32;
@external("wasmos", "ipc_recv") declare function ipc_recv(endpoint: i32): i32;
@external("wasmos", "ipc_try_recv") declare function ipc_try_recv(endpoint: i32): i32;
@external("wasmos", "ipc_send")
declare function ipc_send(destination: i32, source: i32, type: i32, request_id: i32, arg0: i32,
                          arg1: i32, arg2: i32, arg3: i32): i32;
@external("wasmos", "ipc_last_field") declare function ipc_last_field(field: i32): i32;
@external("wasmos", "serial_register") declare function serial_register(endpoint: i32): i32;
@external("wasmos", "irq_route_ipc") declare function irq_route_ipc(irq: i32, endpoint: i32): i32;
@external("wasmos", "irq_ack") declare function irq_ack(irq: i32): i32;
@external("wasmos", "io_in8") declare function io_in8(port: i32): i32;
@external("wasmos", "io_out8") declare function io_out8(port: i32, value: i32): i32;
@external("wasmos", "io_wait") declare function io_wait(): i32;
@external("wasmos", "sched_yield") declare function sched_yield(): i32;

let g_endpoint: i32 = -1;
/* The single RX subscriber (the vt service). -1 = none yet. */
let g_subscriber: i32 = -1;

function serial_init_hw(): void {
    io_out8(COM1_IER, 0x00); /* disable interrupts during setup */
    io_out8(COM1_PORT + 3, 0x80); /* DLAB on */
    io_out8(COM1_PORT + 0, 0x01); /* divisor low (115200) */
    io_out8(COM1_PORT + 1, 0x00); /* divisor high */
    io_out8(COM1_PORT + 3, 0x03); /* 8N1, DLAB off */
    io_out8(COM1_PORT + 2, 0xC7); /* FIFO enable, clear, 14-byte threshold */
    io_out8(COM1_PORT + 4, 0x0B); /* DTR|RTS|OUT2 (OUT2 gates IRQ to the PIC) */
    io_out8(COM1_IER, 0x01); /* enable "received data available" interrupt */
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

/* Legacy pull path.  RX is now drained by the IRQ handler and forwarded to the
 * subscribed vt, so this reports empty (the kernel console_read fallback keeps
 * working during early boot before a subscriber exists). */
function handle_read(request_id: i32, source: i32): void {
    let char_code = read_port();
    if (char_code >= 0) {
        send_response(source, request_id, char_code, SERIAL_READ_STATUS_CHAR);
        return;
    }
    send_response(source, request_id, 0, SERIAL_READ_STATUS_EMPTY);
}

/* Forward one RX byte to the subscribed vt as VT_IPC_SERIAL_INPUT_REQ. */
function push_rx_byte(byte: i32): void {
    if (g_subscriber < 0) {
        return;
    }
    let packed = (1 << 24) | (byte & 0xFF);
    ipc_send(g_subscriber, g_endpoint, VT_IPC_SERIAL_INPUT_REQ, 0, packed, 0, 0, 0);
}

/* Drain all bytes currently in the UART RX FIFO to the subscriber. */
function drain_rx(): void {
    for (;;) {
        let c = read_port();
        if (c < 0) {
            break;
        }
        push_rx_byte(c);
    }
}

function handle_subscribe(request_id: i32, source: i32): void {
    if (source < 0) {
        return;
    }
    g_subscriber = source;
    ipc_send(source, g_endpoint, SERIAL_IPC_SUBSCRIBE_RESP, request_id, 0, 0, 0, 0);
}

/* Register a discoverable service name so the vt can look us up and subscribe. */
function register_service(proc_endpoint: i32): bool {
    let name = 0x006E6973; /* "sin\0" - serial input */
    let req_id = 1;
    if (ipc_send(proc_endpoint, g_endpoint, SVC_IPC_REGISTER_REQ, req_id, name, 0, 0, 0) != 0) {
        return false;
    }
    if (ipc_recv(g_endpoint) != 1) {
        return false;
    }
    return ipc_last_field(0) == SVC_IPC_REGISTER_RESP && ipc_last_field(1) == req_id &&
           ipc_last_field(2) == 0;
}

export function initialize(_proc_endpoint: i32, _module_count: i32, _arg2: i32, _arg3: i32): i32 {
    // proc.endpoint now comes from the spawn-info contract, not an entry arg.
    _proc_endpoint = startup.procEndpoint();

    g_endpoint = ipc_create_endpoint();
    if (g_endpoint < 0) {
        std.printf("[serial] endpoint failure\n");
        return -1;
    }
    /* Kernel remote-driver hook (keeps console_read's fallback path wired). */
    if (serial_register(g_endpoint) != 0) {
        std.printf("[serial] register failure\n");
        return -1;
    }
    /* Discoverable name for the vt subscription. */
    if (!register_service(_proc_endpoint)) {
        std.printf("[serial] svc register failure\n");
        return -1;
    }

    serial_init_hw();

    let irq_ok: i32 = irq_route_ipc(SERIAL_IRQ, g_endpoint);
    if (irq_ok != 0) {
        std.printf("[serial] IRQ route failed, falling back to polling\n");
    } else {
        std.printf("[serial] driver ready (IRQ-driven)\n");
    }
    ipc_send(_proc_endpoint, g_endpoint, PROC_IPC_NOTIFY_READY, 0, 0, 0, 0, 0);

    if (irq_ok != 0) {
        /* Polling fallback: interleave IPC servicing with RX polling. */
        io_out8(COM1_IER, 0x00); /* no interrupts; we poll */
        for (;;) {
            let rc = ipc_try_recv(g_endpoint);
            if (rc == 1) {
                let kind = ipc_last_field(IPC_FIELD_TYPE);
                let request_id = ipc_last_field(IPC_FIELD_REQUEST_ID);
                let source = ipc_last_field(IPC_FIELD_SOURCE);
                if (kind == SERIAL_DRIVER_WRITE_REQ) {
                    handle_write(request_id, source);
                } else if (kind == SERIAL_IPC_SUBSCRIBE_REQ) {
                    handle_subscribe(request_id, source);
                } else if (kind == SERIAL_DRIVER_READ_REQ) {
                    handle_read(request_id, source);
                }
            }
            drain_rx();
            io_wait();
            sched_yield();
        }
        return 0;
    }

    for (;;) {
        /* Block until a client message or an RX IRQ event arrives. */
        if (ipc_recv(g_endpoint) != 1) {
            continue;
        }
        let kind = ipc_last_field(IPC_FIELD_TYPE);
        let request_id = ipc_last_field(IPC_FIELD_REQUEST_ID);
        let source = ipc_last_field(IPC_FIELD_SOURCE);
        if (kind == SERIAL_IPC_IRQ_EVENT) {
            drain_rx();
            /* Ack after reading the FIFO so the line deasserts before unmask. */
            irq_ack(SERIAL_IRQ);
        } else if (kind == SERIAL_DRIVER_WRITE_REQ) {
            handle_write(request_id, source);
        } else if (kind == SERIAL_IPC_SUBSCRIBE_REQ) {
            handle_subscribe(request_id, source);
        } else if (kind == SERIAL_DRIVER_READ_REQ) {
            handle_read(request_id, source);
        } else {
            send_response(source, request_id, 0, SERIAL_READ_STATUS_ERROR);
        }
    }

    return 0;
}
