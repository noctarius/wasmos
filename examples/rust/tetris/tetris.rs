//! WASMOS Tetris — a two-player, networked Tetris for the gfx compositor.
//!
//! * Graphics: talks to the `gfx` compositor directly over its IPC protocol
//!   (create window, allocate a shared BGRA32 buffer, present). Rendering is
//!   double-buffered: every frame is composed into an app-owned back buffer and
//!   then blitted into the shared buffer in one pass, so the compositor never
//!   samples a half-drawn frame.
//! * Input: keyboard events pushed by the compositor (translated ASCII, so the
//!   controls are WASD + space — arrow keys are not distinguishable through this
//!   API); pointer clicks drive the start menu buttons.
//! * Networking: two instances connect over net-stack TCP (via the C transport
//!   shim in `net_shim.c`). One instance hosts (listen/accept), the other joins
//!   (connect to the SLIRP host gateway). Each side plays its own board, streams
//!   its board state to the peer, and cleared lines send garbage rows across.
//!
//! `#![no_std]`, no heap: all state is fixed arrays / `static mut` singletons.
#![no_std]
#![no_main]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

// ---------------------------------------------------------------------------
// Raw host imports and the C net-shim ABI.
// ---------------------------------------------------------------------------

#[link(wasm_import_module = "wasmos")]
unsafe extern "C" {
    fn console_write(ptr: i32, len: i32) -> i32;
    fn proc_exit(status: i32) -> i32;
    fn sched_ticks() -> i32;
    fn ipc_create_endpoint() -> i32;
    fn ipc_endpoint_owner(endpoint: i32) -> i32;
    fn ipc_send(dst: i32, src: i32, ty: i32, rid: i32, a0: i32, a1: i32, a2: i32, a3: i32) -> i32;
    fn ipc_select_one(endpoint: i32) -> i32;
    fn ipc_drain(endpoint: i32) -> i32;
    fn ipc_select_create() -> i32;
    fn ipc_select_add(select_id: i32, endpoint: i32) -> i32;
    fn ipc_select_wait_timeout(select_id: i32, timeout_ms: i32) -> i32;
    fn ipc_last_field(field: i32) -> i32;
    fn shmem_create(pages: i32, flags: i32) -> i32;
    fn shmem_grant(id: i32, target_pid: i32) -> i32;
    fn shmem_map_auto(id: i32, size: i32) -> i32;
    fn shmem_flush(id: i32, ptr: i32, size: i32) -> i32;
    fn spawn_info_buffer() -> i32;
    fn xfer_buffer_read(buffer_id: i32, ptr: i32, len: i32, offset: i32) -> i32;
}

// Provided by net_shim.c (linked object).
unsafe extern "C" {
    // Non-blocking handshake: *_begin arms it and returns immediately; the game
    // loop calls tnet_net_advance whenever the reply-endpoint doorbell wakes it.
    // Return codes: begin -> 0 armed / <0 setup error; advance -> 1 ready /
    // 0 still handshaking / <0 failed.
    fn tnet_join_begin(proc_ep: i32, addr_no: u32, port: u32) -> i32;
    fn tnet_host_begin(proc_ep: i32, port: u32) -> i32;
    fn tnet_net_advance() -> i32;
    fn tnet_reply_ep() -> i32;
    fn tnet_send(data: *const u8, len: i32) -> i32;
    fn tnet_poll(buf: *mut u8, cap: i32) -> i32;
    fn tnet_close();
}

// IPC "last message" field indices (see wasmos ipc host ABI).
const F_TYPE: i32 = 0;
const F_REQID: i32 = 1;
const F_ARG0: i32 = 2;
const F_ARG1: i32 = 3;
const F_ARG2: i32 = 6;
const F_ARG3: i32 = 7;

// Service registry opcodes.
const SVC_IPC_LOOKUP_REQ: i32 = 0x221;
const SVC_IPC_LOOKUP_RESP: i32 = 0x2A1;

// GFX compositor protocol.
const GFX_IPC_ABI_MAGIC: i32 = 0x4746_5850;
const GFX_IPC_ABI_VERSION: i32 = 1;
const GFX_IPC_CREATE_WINDOW: i32 = 0x0200;
const GFX_IPC_ALLOC_SHARED_BUFFER: i32 = 0x0203;
const GFX_IPC_PRESENT_WINDOW: i32 = 0x0205;
const GFX_IPC_PUSH_EVENT: i32 = 0x0206;
const GFX_IPC_FOCUS_WINDOW: i32 = 0x020A;
const GFX_IPC_GET_DISPLAY_INFO: i32 = 0x020C;
const GFX_IPC_MOVE_WINDOW: i32 = 0x020D;
const GFX_IPC_SET_WINDOW_TITLE: i32 = 0x020E;
const GFX_IPC_RESP: i32 = 0x0280;
const WASMOS_ERR_NONE: i32 = 0;

const GFX_EVENT_KEY: i32 = 3;
const GFX_EVENT_POINTER: i32 = 4;
const GFX_EVENT_CLOSE_REQUEST: i32 = 5;

// SLIRP host gateway 10.0.2.2 as a network-order IPv4 word (octet a in low byte),
// and the gameplay port. The host VM forwards this port to itself; the joining VM
// reaches the host through its own gateway.
const PEER_ADDR_V4: u32 = 0x0202_000A;
const GAME_PORT: u32 = 7000;

// ---------------------------------------------------------------------------
// Small logging helper.
// ---------------------------------------------------------------------------

fn log(msg: &[u8]) {
    unsafe {
        let _ = console_write(msg.as_ptr() as i32, msg.len() as i32);
    }
}

fn pack_name16(name: &[u8]) -> [i32; 4] {
    let mut out = [0i32; 4];
    let mut i = 0;
    while i < name.len() && i < 16 {
        let slot = i / 4;
        let shift = (i % 4) * 8;
        out[slot] |= (name[i] as i32) << shift;
        i += 1;
    }
    out
}

// ---------------------------------------------------------------------------
// Window / compositor client.
// ---------------------------------------------------------------------------

// Window size, chosen to sit comfortably inside the 1280x800 framebuffer with
// two 32px-cell boards side by side (chrome adds ~26px, so H stays under 800).
const W: i32 = 856;
const H: i32 = 744;

struct Window {
    proc_ep: i32,
    gfx_ep: i32,
    event_ep: i32,
    reply_ep: i32,
    window_id: i32,
    buffer_id: i32,
    shmem_id: i32,
    base: *mut u8, // mapped shared buffer (BGRA32)
    stride: i32,   // bytes per row
    rid: i32,
    // deferred input state harvested from pushed events
    key: u8, // last key-down ASCII (0 = none), consumed by caller
    close: bool,
    ptr_x: i32,
    ptr_y: i32,
    ptr_buttons: u32,
    ptr_prev: u32,
}

// App-owned back buffer for double-buffered composition.
static mut BACK: [u32; (W * H) as usize] = [0; (W * H) as usize];

impl Window {
    /// Wait for the reply matching `rid` on `ep`. Blocks (sleeps in-kernel) on
    /// `ipc_select_one` for each incoming message rather than busy-polling;
    /// bounded so a lost reply during setup fails instead of hanging forever.
    fn wait_reply(&self, ep: i32, rid: i32) -> Option<(i32, i32, i32, i32, i32)> {
        for _ in 0..64 {
            if unsafe { ipc_select_one(ep) } != 1 {
                return None;
            }
            let got = unsafe { ipc_last_field(F_REQID) };
            if got == rid {
                let ty = unsafe { ipc_last_field(F_TYPE) };
                let a0 = unsafe { ipc_last_field(F_ARG0) };
                let a1 = unsafe { ipc_last_field(F_ARG1) };
                let a2 = unsafe { ipc_last_field(F_ARG2) };
                let a3 = unsafe { ipc_last_field(F_ARG3) };
                return Some((ty, a0, a1, a2, a3));
            }
        }
        None
    }

    fn next_rid(&mut self) -> i32 {
        self.rid += 1;
        self.rid
    }

    fn lookup(&mut self, name: &[u8]) -> i32 {
        let packed = pack_name16(name);
        // The compositor is up before this app launches (sysinit `wait-svc gfx`),
        // so a small bounded retry suffices; each attempt blocks for its reply.
        for _ in 0..64 {
            let rid = self.next_rid();
            unsafe {
                let _ = ipc_send(
                    self.proc_ep,
                    self.reply_ep,
                    SVC_IPC_LOOKUP_REQ,
                    rid,
                    packed[0],
                    packed[1],
                    packed[2],
                    packed[3],
                );
            }
            if let Some((ty, a0, _, _, _)) = self.wait_reply(self.reply_ep, rid) {
                if ty == SVC_IPC_LOOKUP_RESP && a0 != -1 {
                    return a0;
                }
            }
        }
        -1
    }

    fn open(proc_ep: i32) -> Option<Window> {
        let event_ep = unsafe { ipc_create_endpoint() };
        let reply_ep = unsafe { ipc_create_endpoint() };
        if event_ep < 0 || reply_ep < 0 {
            return None;
        }
        let mut w = Window {
            proc_ep,
            gfx_ep: -1,
            event_ep,
            reply_ep,
            window_id: -1,
            buffer_id: 0,
            shmem_id: -1,
            base: core::ptr::null_mut(),
            stride: W * 4,
            rid: 0,
            key: 0,
            close: false,
            ptr_x: 0,
            ptr_y: 0,
            ptr_buttons: 0,
            ptr_prev: 0,
        };
        w.gfx_ep = w.lookup(b"gfx");
        if w.gfx_ep <= 0 {
            log(b"[tetris] no gfx service\n");
            return None;
        }
        let gfx_owner = unsafe { ipc_endpoint_owner(w.gfx_ep) };

        // Create the window from the event endpoint so it becomes the owner the
        // compositor pushes events to; the reply comes back on that endpoint.
        let hdr = (GFX_IPC_ABI_VERSION << 16) | (GFX_IPC_CREATE_WINDOW & 0xFFFF);
        unsafe {
            let _ = ipc_send(
                w.gfx_ep,
                w.event_ep,
                GFX_IPC_CREATE_WINDOW,
                1,
                W,
                H,
                GFX_IPC_ABI_MAGIC,
                hdr,
            );
        }
        match w.wait_reply(w.event_ep, 1) {
            Some((ty, a0, a1, _, _)) if ty == GFX_IPC_RESP && a0 == WASMOS_ERR_NONE => {
                w.window_id = a1;
            }
            _ => {
                log(b"[tetris] create window failed\n");
                return None;
            }
        }

        // Position the window so it fits fully on screen. New windows are
        // cascaded by the compositor and can land partly off the bottom edge;
        // it then clamps their height to the framebuffer, which would leave the
        // window content size out of sync with this app's fixed buffer. Query the
        // display, center the window, and move it there before allocating.
        let rid = w.next_rid();
        unsafe {
            let _ = ipc_send(
                w.gfx_ep,
                w.reply_ep,
                GFX_IPC_GET_DISPLAY_INFO,
                rid,
                0,
                0,
                0,
                0,
            );
        }
        if let Some((ty, a0, a1, a2, _)) = w.wait_reply(w.reply_ep, rid) {
            if ty == GFX_IPC_RESP && a0 == WASMOS_ERR_NONE {
                let fb_w = a1;
                let fb_h = a2;
                // Content top sits ~26px below the window's y (title bar + border).
                let px = if fb_w > W { (fb_w - W) / 2 } else { 0 };
                let py = if fb_h > H + 32 {
                    (fb_h - H - 32) / 2
                } else {
                    4
                };
                let rid = w.next_rid();
                unsafe {
                    let _ = ipc_send(
                        w.gfx_ep,
                        w.reply_ep,
                        GFX_IPC_MOVE_WINDOW,
                        rid,
                        w.window_id,
                        px,
                        py,
                        0,
                    );
                }
                let _ = w.wait_reply(w.reply_ep, rid);
            }
        }

        // Allocate the shared drawing buffer and map it.
        let rid = w.next_rid();
        unsafe {
            let _ = ipc_send(
                w.gfx_ep,
                w.reply_ep,
                GFX_IPC_ALLOC_SHARED_BUFFER,
                rid,
                w.window_id,
                W,
                H,
                0,
            );
        }
        match w.wait_reply(w.reply_ep, rid) {
            Some((ty, a0, a1, a2, a3)) if ty == GFX_IPC_RESP && a0 == WASMOS_ERR_NONE => {
                w.buffer_id = a1;
                w.shmem_id = a2;
                w.stride = a3;
            }
            _ => {
                log(b"[tetris] alloc buffer failed\n");
                return None;
            }
        }
        // Force-commit the whole back buffer before mapping the shared buffer.
        // The WARP shmem mapper places the mapped window just above the process's
        // currently *committed* linear memory; the large `BACK` static is not
        // committed until first written, so without this the shared-buffer window
        // is placed overlapping `BACK` and the present() blit corrupts itself
        // (content tears/duplicates). Touching one byte per page commits it so the
        // window lands safely above.
        unsafe {
            let p = core::ptr::addr_of_mut!(BACK) as *mut u8;
            let total = (W * H * 4) as usize;
            let mut o = 0usize;
            while o < total {
                core::ptr::write_volatile(p.add(o), 0u8);
                o += 4096;
            }
            core::ptr::write_volatile(p.add(total - 1), 0u8);
        }

        let byte_len = w.stride * H;
        let map_len = (byte_len + 4095) & !4095;
        let off = unsafe { shmem_map_auto(w.shmem_id, map_len) };
        if off <= 0 {
            log(b"[tetris] map buffer failed\n");
            return None;
        }
        w.base = off as *mut u8;

        w.set_title(gfx_owner, b"WASMOS Tetris");
        Some(w)
    }

    fn set_title(&mut self, gfx_owner: i32, title: &[u8]) {
        let sid = unsafe { shmem_create(1, 0) };
        if sid <= 0 {
            return;
        }
        if unsafe { shmem_grant(sid, gfx_owner) } != 0 {
            return;
        }
        let off = unsafe { shmem_map_auto(sid, 4096) };
        if off <= 0 {
            return;
        }
        let p = off as *mut u8;
        let n = if title.len() > 47 { 47 } else { title.len() };
        unsafe {
            for i in 0..n {
                *p.add(i) = title[i];
            }
            *p.add(n) = 0;
            let _ = shmem_flush(sid, off, (n + 1) as i32);
        }
        let rid = self.next_rid();
        unsafe {
            let _ = ipc_send(
                self.gfx_ep,
                self.reply_ep,
                GFX_IPC_SET_WINDOW_TITLE,
                rid,
                self.window_id,
                sid,
                n as i32,
                0,
            );
        }
        let _ = self.wait_reply(self.reply_ep, rid);
    }

    /// Blit the back buffer into the shared buffer and present.
    ///
    /// No shmem_flush: `base` is the mapped window, i.e. the shared region's
    /// own physical pages, so writing through it IS the shared buffer. A flush
    /// here would copy those pages onto themselves once a frame -- 2.4 MB of
    /// pointless copying at frame rate.
    fn present(&mut self) {
        unsafe {
            let src = core::ptr::addr_of!(BACK) as *const u32;
            for y in 0..H {
                let row = self.base.add((y * self.stride) as usize) as *mut u32;
                let srow = src.add((y * W) as usize);
                for x in 0..W {
                    *row.add(x as usize) = *srow.add(x as usize);
                }
            }
        }
        let rid = self.next_rid();
        unsafe {
            let _ = ipc_send(
                self.gfx_ep,
                self.reply_ep,
                GFX_IPC_PRESENT_WINDOW,
                rid,
                self.window_id,
                self.buffer_id,
                0,
                0,
            );
        }
        let _ = self.wait_reply(self.reply_ep, rid);
    }

    /// Drain pushed compositor events (non-blocking). Updates key/close/pointer.
    fn pump(&mut self) {
        self.ptr_prev = self.ptr_buttons;
        self.key = 0;
        loop {
            // Non-blocking drain: ipc_select_one BLOCKS on WARP, ipc_drain does
            // not. Pull each queued pushed event and stop when the mailbox empties.
            if unsafe { ipc_drain(self.event_ep) } != 1 {
                break;
            }
            let ty = unsafe { ipc_last_field(F_TYPE) };
            if ty != GFX_IPC_PUSH_EVENT {
                continue;
            }
            let ev = unsafe { ipc_last_field(F_ARG1) };
            let a2 = unsafe { ipc_last_field(F_ARG2) };
            let a3 = unsafe { ipc_last_field(F_ARG3) };
            match ev {
                GFX_EVENT_KEY => {
                    let down = (a3 & 1) != 0;
                    let code = (a2 & 0xFF) as u8;
                    if down && code != 0 {
                        self.key = code;
                    }
                }
                GFX_EVENT_POINTER => {
                    if a2 == self.window_id {
                        let p = a3 as u32;
                        self.ptr_x = (p & 0xFFF) as i32;
                        self.ptr_y = ((p >> 12) & 0xFFF) as i32;
                        self.ptr_buttons = (p >> 24) & 0xFF;
                    }
                }
                GFX_EVENT_CLOSE_REQUEST => {
                    if a2 == self.window_id {
                        self.close = true;
                    }
                }
                _ => {}
            }
        }
    }

    fn left_click(&self) -> bool {
        (self.ptr_buttons & 1) != 0 && (self.ptr_prev & 1) == 0
    }

    /// Ask the compositor to give this window keyboard focus. A launched game
    /// should own the keyboard; the compositor only auto-focuses on first
    /// present, and another already-focused app would otherwise keep it.
    fn focus(&mut self) {
        let rid = self.next_rid();
        unsafe {
            let _ = ipc_send(
                self.gfx_ep,
                self.reply_ep,
                GFX_IPC_FOCUS_WINDOW,
                rid,
                self.window_id,
                0,
                0,
                0,
            );
        }
        let _ = self.wait_reply(self.reply_ep, rid);
    }
}

// ---------------------------------------------------------------------------
// Software rendering into the back buffer.
// ---------------------------------------------------------------------------

fn px(x: i32, y: i32, color: u32) {
    if x < 0 || y < 0 || x >= W || y >= H {
        return;
    }
    unsafe {
        let p = core::ptr::addr_of_mut!(BACK) as *mut u32;
        *p.add((y * W + x) as usize) = color;
    }
}

fn fill_rect(x: i32, y: i32, w: i32, h: i32, color: u32) {
    let x0 = x.max(0);
    let y0 = y.max(0);
    let x1 = (x + w).min(W);
    let y1 = (y + h).min(H);
    let mut yy = y0;
    while yy < y1 {
        let mut xx = x0;
        while xx < x1 {
            px(xx, yy, color);
            xx += 1;
        }
        yy += 1;
    }
}

fn stroke_rect(x: i32, y: i32, w: i32, h: i32, color: u32) {
    fill_rect(x, y, w, 1, color);
    fill_rect(x, y + h - 1, w, 1, color);
    fill_rect(x, y, 1, h, color);
    fill_rect(x + w - 1, y, 1, h, color);
}

fn clear(color: u32) {
    fill_rect(0, 0, W, H, color);
}

// A compact 5x7 uppercase bitmap font (rows top→bottom, bit4 = leftmost pixel).
// Covers the characters used by the UI: A-Z, 0-9, space, ':', '-', '.', '/'.
fn glyph(ch: u8) -> [u8; 7] {
    match ch {
        b'A' => [0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11],
        b'B' => [0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E],
        b'C' => [0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E],
        b'D' => [0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E],
        b'E' => [0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F],
        b'F' => [0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10],
        b'G' => [0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F],
        b'H' => [0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11],
        b'I' => [0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E],
        b'J' => [0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C],
        b'K' => [0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11],
        b'L' => [0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F],
        b'M' => [0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11],
        b'N' => [0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11],
        b'O' => [0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E],
        b'P' => [0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10],
        b'Q' => [0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D],
        b'R' => [0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11],
        b'S' => [0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E],
        b'T' => [0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04],
        b'U' => [0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E],
        b'V' => [0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04],
        b'W' => [0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11],
        b'X' => [0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11],
        b'Y' => [0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04],
        b'Z' => [0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F],
        b'0' => [0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E],
        b'1' => [0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E],
        b'2' => [0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F],
        b'3' => [0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E],
        b'4' => [0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02],
        b'5' => [0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E],
        b'6' => [0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E],
        b'7' => [0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08],
        b'8' => [0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E],
        b'9' => [0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C],
        b':' => [0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00],
        b'-' => [0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00],
        b'.' => [0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C],
        b'/' => [0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10],
        _ => [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
    }
}

fn draw_char(x: i32, y: i32, ch: u8, scale: i32, color: u32) {
    let g = glyph(ch);
    for row in 0..7 {
        let bits = g[row as usize];
        for col in 0..5 {
            if (bits >> (4 - col)) & 1 != 0 {
                fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

fn draw_text(x: i32, y: i32, text: &[u8], scale: i32, color: u32) {
    let mut cx = x;
    for &ch in text {
        let up = if ch >= b'a' && ch <= b'z' {
            ch - 32
        } else {
            ch
        };
        draw_char(cx, y, up, scale, color);
        cx += 6 * scale;
    }
}

/// Render an unsigned number right-aligned at (x_right, y). Returns nothing.
fn draw_number(x_right: i32, y: i32, mut value: u32, scale: i32, color: u32) {
    let mut digits = [0u8; 10];
    let mut n = 0;
    loop {
        digits[n] = b'0' + (value % 10) as u8;
        value /= 10;
        n += 1;
        if value == 0 {
            break;
        }
    }
    let mut cx = x_right - (n as i32) * 6 * scale;
    for i in (0..n).rev() {
        draw_char(cx, y, digits[i], scale, color);
        cx += 6 * scale;
    }
}

// ---------------------------------------------------------------------------
// Tetris engine.
// ---------------------------------------------------------------------------

const BW: usize = 10; // board columns
const BH: usize = 20; // board rows

// Piece colors (0xAARRGGBB); index 0 unused, 8 = garbage.
const COLORS: [u32; 9] = [
    0xFF10_1820, // 0 empty (unused as a fill)
    0xFF2CD3_E1, // 1 I cyan
    0xFFF2C1_4E, // 2 O yellow
    0xFFB05C_D6, // 3 T purple
    0xFF5FCB_65, // 4 S green
    0xFFE0524_8, // 5 Z red
    0xFF4C8B_F2, // 6 J blue
    0xFFE0894_0, // 7 L orange
    0xFF6A737_D, // 8 garbage gray
];

// 7 tetrominoes × 4 rotations × 4 blocks, each block an (x,y) offset in a 4-box.
const PIECES: [[[(i8, i8); 4]; 4]; 7] = [
    // I
    [
        [(0, 1), (1, 1), (2, 1), (3, 1)],
        [(2, 0), (2, 1), (2, 2), (2, 3)],
        [(0, 2), (1, 2), (2, 2), (3, 2)],
        [(1, 0), (1, 1), (1, 2), (1, 3)],
    ],
    // O
    [
        [(1, 0), (2, 0), (1, 1), (2, 1)],
        [(1, 0), (2, 0), (1, 1), (2, 1)],
        [(1, 0), (2, 0), (1, 1), (2, 1)],
        [(1, 0), (2, 0), (1, 1), (2, 1)],
    ],
    // T
    [
        [(1, 0), (0, 1), (1, 1), (2, 1)],
        [(1, 0), (1, 1), (2, 1), (1, 2)],
        [(0, 1), (1, 1), (2, 1), (1, 2)],
        [(1, 0), (0, 1), (1, 1), (1, 2)],
    ],
    // S
    [
        [(1, 0), (2, 0), (0, 1), (1, 1)],
        [(1, 0), (1, 1), (2, 1), (2, 2)],
        [(1, 1), (2, 1), (0, 2), (1, 2)],
        [(0, 0), (0, 1), (1, 1), (1, 2)],
    ],
    // Z
    [
        [(0, 0), (1, 0), (1, 1), (2, 1)],
        [(2, 0), (1, 1), (2, 1), (1, 2)],
        [(0, 1), (1, 1), (1, 2), (2, 2)],
        [(1, 0), (0, 1), (1, 1), (0, 2)],
    ],
    // J
    [
        [(0, 0), (0, 1), (1, 1), (2, 1)],
        [(1, 0), (2, 0), (1, 1), (1, 2)],
        [(0, 1), (1, 1), (2, 1), (2, 2)],
        [(1, 0), (1, 1), (0, 2), (1, 2)],
    ],
    // L
    [
        [(2, 0), (0, 1), (1, 1), (2, 1)],
        [(1, 0), (1, 1), (1, 2), (2, 2)],
        [(0, 1), (1, 1), (2, 1), (0, 2)],
        [(0, 0), (1, 0), (1, 1), (1, 2)],
    ],
];

struct Rng(u32);
impl Rng {
    fn next(&mut self) -> u32 {
        self.0 = self.0.wrapping_mul(1664525).wrapping_add(1013904223);
        self.0
    }
}

struct Board {
    cells: [[u8; BW]; BH],
    kind: usize,
    rot: usize,
    x: i32,
    y: i32,
    score: u32,
    lines: u32,
    garbage_out: u16,
    over: bool,
    pending_garbage: u32,
    rng: Rng,
}

impl Board {
    fn new(seed: u32) -> Board {
        let mut b = Board {
            cells: [[0u8; BW]; BH],
            kind: 0,
            rot: 0,
            x: 0,
            y: 0,
            score: 0,
            lines: 0,
            garbage_out: 0,
            over: false,
            pending_garbage: 0,
            rng: Rng(seed | 1),
        };
        b.spawn();
        b
    }

    fn spawn(&mut self) {
        self.kind = (self.rng.next() % 7) as usize;
        self.rot = 0;
        self.x = 3;
        self.y = 0;
        if self.collides(self.kind, self.rot, self.x, self.y) {
            self.over = true;
        }
    }

    fn collides(&self, kind: usize, rot: usize, ox: i32, oy: i32) -> bool {
        for &(bx, by) in PIECES[kind][rot].iter() {
            let cx = ox + bx as i32;
            let cy = oy + by as i32;
            if cx < 0 || cx >= BW as i32 || cy >= BH as i32 {
                return true;
            }
            if cy >= 0 && self.cells[cy as usize][cx as usize] != 0 {
                return true;
            }
        }
        false
    }

    fn lock(&mut self) {
        let color = (self.kind + 1) as u8;
        for &(bx, by) in PIECES[self.kind][self.rot].iter() {
            let cx = self.x + bx as i32;
            let cy = self.y + by as i32;
            if cy >= 0 && cy < BH as i32 && cx >= 0 && cx < BW as i32 {
                self.cells[cy as usize][cx as usize] = color;
            }
        }
    }

    fn clear_lines(&mut self) -> u32 {
        let mut cleared = 0;
        let mut y = BH as i32 - 1;
        while y >= 0 {
            let mut full = true;
            for x in 0..BW {
                if self.cells[y as usize][x] == 0 {
                    full = false;
                    break;
                }
            }
            if full {
                // shift everything above down by one
                let mut yy = y as usize;
                while yy > 0 {
                    self.cells[yy] = self.cells[yy - 1];
                    yy -= 1;
                }
                self.cells[0] = [0u8; BW];
                cleared += 1;
                // re-test the same row index (now holds the shifted-down row)
            } else {
                y -= 1;
            }
        }
        cleared
    }

    fn add_garbage(&mut self, rows: u32) {
        for _ in 0..rows {
            // shift board up by one, drop the top row
            for y in 0..BH - 1 {
                self.cells[y] = self.cells[y + 1];
            }
            let hole = (self.rng.next() % BW as u32) as usize;
            let mut row = [8u8; BW];
            row[hole] = 0;
            self.cells[BH - 1] = row;
        }
    }

    fn move_h(&mut self, dx: i32) {
        if !self.collides(self.kind, self.rot, self.x + dx, self.y) {
            self.x += dx;
        }
    }

    fn rotate(&mut self) {
        let nr = (self.rot + 1) % 4;
        // basic wall kick: try in place, then nudge left/right
        for &dx in &[0i32, -1, 1, -2, 2] {
            if !self.collides(self.kind, nr, self.x + dx, self.y) {
                self.rot = nr;
                self.x += dx;
                return;
            }
        }
    }

    /// Step one row down; on landing, lock/clear/spawn. Returns lines cleared.
    fn step_down(&mut self) -> u32 {
        if !self.collides(self.kind, self.rot, self.x, self.y + 1) {
            self.y += 1;
            return 0;
        }
        self.lock();
        let cleared = self.clear_lines();
        self.apply_scoring(cleared);
        if self.pending_garbage > 0 {
            let g = self.pending_garbage;
            self.pending_garbage = 0;
            self.add_garbage(g);
        }
        self.spawn();
        cleared
    }

    fn hard_drop(&mut self) -> u32 {
        while !self.collides(self.kind, self.rot, self.x, self.y + 1) {
            self.y += 1;
            self.score += 2;
        }
        self.step_down()
    }

    fn apply_scoring(&mut self, cleared: u32) {
        if cleared == 0 {
            return;
        }
        self.lines += cleared;
        self.score += match cleared {
            1 => 100,
            2 => 300,
            3 => 500,
            _ => 800,
        };
        // Garbage sent to opponent: 0/1/2/4 for single/double/triple/tetris.
        let attack = match cleared {
            2 => 1,
            3 => 2,
            4 => 4,
            _ => 0,
        };
        self.garbage_out = self.garbage_out.wrapping_add(attack);
    }
}

// ---------------------------------------------------------------------------
// Wire protocol (fixed 212-byte frames).
// ---------------------------------------------------------------------------

const PKT_LEN: usize = 212;
const PKT_MAGIC: u8 = b'T';
const PKT_VER: u8 = 1;

struct PeerState {
    cells: [[u8; BW]; BH],
    score: u32,
    lines: u32,
    garbage_out: u16,
    last_garbage_seen: u16,
    over: bool,
    seen: bool,
}

impl PeerState {
    fn new() -> PeerState {
        PeerState {
            cells: [[0u8; BW]; BH],
            score: 0,
            lines: 0,
            garbage_out: 0,
            last_garbage_seen: 0,
            over: false,
            seen: false,
        }
    }
}

fn encode_packet(b: &Board, seq: u8, out: &mut [u8; PKT_LEN]) {
    out[0] = PKT_MAGIC;
    out[1] = PKT_VER;
    out[2] = if b.over { 1 } else { 0 };
    out[3] = seq;
    out[4] = (b.lines & 0xFF) as u8;
    out[5] = ((b.lines >> 8) & 0xFF) as u8;
    out[6] = (b.garbage_out & 0xFF) as u8;
    out[7] = ((b.garbage_out >> 8) & 0xFF) as u8;
    out[8] = (b.score & 0xFF) as u8;
    out[9] = ((b.score >> 8) & 0xFF) as u8;
    out[10] = ((b.score >> 16) & 0xFF) as u8;
    out[11] = ((b.score >> 24) & 0xFF) as u8;
    let mut i = 12;
    for y in 0..BH {
        for x in 0..BW {
            out[i] = b.cells[y][x];
            i += 1;
        }
    }
}

/// Parse one 212-byte frame into the peer state; returns garbage delta to apply.
fn decode_packet(buf: &[u8], peer: &mut PeerState) {
    peer.over = buf[2] & 1 != 0;
    peer.lines = buf[4] as u32 | ((buf[5] as u32) << 8);
    peer.garbage_out = buf[6] as u16 | ((buf[7] as u16) << 8);
    peer.score = buf[8] as u32
        | ((buf[9] as u32) << 8)
        | ((buf[10] as u32) << 16)
        | ((buf[11] as u32) << 24);
    let mut i = 12;
    for y in 0..BH {
        for x in 0..BW {
            peer.cells[y][x] = buf[i];
            i += 1;
        }
    }
    peer.seen = true;
}

// ---------------------------------------------------------------------------
// Layout + rendering of the game screens.
// ---------------------------------------------------------------------------

const CELL: i32 = 32;
const BOARD_PX_W: i32 = BW as i32 * CELL; // 320
const BOARD_PX_H: i32 = BH as i32 * CELL; // 640
const HEADER: i32 = 80;
const MARGIN: i32 = 24;
const LEFT_X: i32 = MARGIN; // your board (versus)
const RIGHT_X: i32 = W - MARGIN - BOARD_PX_W; // opponent board (versus)
const CENTER_X: i32 = LEFT_X + BOARD_PX_W + 20; // center status column
const SOLO_X: i32 = (W - BOARD_PX_W) / 2; // centered board (single player)

const BG: u32 = 0xFF14_1B24;
const PANEL: u32 = 0xFF1E2A3_A;
const GRID: u32 = 0xFF2A38_4C;
const TEXT: u32 = 0xFFE6EDF_5;
const DIM: u32 = 0xFF8A97A_8;

fn draw_cells(ox: i32, oy: i32, cells: &[[u8; BW]; BH]) {
    fill_rect(ox - 2, oy - 2, BOARD_PX_W + 4, BOARD_PX_H + 4, PANEL);
    for y in 0..BH {
        for x in 0..BW {
            let cx = ox + x as i32 * CELL;
            let cy = oy + y as i32 * CELL;
            let v = cells[y][x];
            if v == 0 {
                fill_rect(cx, cy, CELL, CELL, BG);
                stroke_rect(cx, cy, CELL, CELL, GRID);
            } else {
                let c = COLORS[v as usize];
                fill_rect(cx, cy, CELL, CELL, c);
                stroke_rect(cx, cy, CELL, CELL, 0xFF0E14_1C);
                fill_rect(cx + 2, cy + 2, CELL - 4, 2, 0x33FF_FFFF | (c & 0xFF00_0000));
            }
        }
    }
}

fn draw_active(ox: i32, oy: i32, b: &Board) {
    if b.over {
        return;
    }
    let c = COLORS[b.kind + 1];
    for &(bx, by) in PIECES[b.kind][b.rot].iter() {
        let cx = ox + (b.x + bx as i32) * CELL;
        let cy = oy + (b.y + by as i32) * CELL;
        fill_rect(cx, cy, CELL, CELL, c);
        stroke_rect(cx, cy, CELL, CELL, 0xFF0E14_1C);
    }
}

/// Pixel width of `len` characters at the given text scale (advance = 6*scale).
fn text_width(len: usize, scale: i32) -> i32 {
    len as i32 * 6 * scale
}

#[derive(Clone, Copy, PartialEq)]
enum Mode {
    Solo,
    Host,
    Join,
}

fn render_game(me: &Board, peer: &PeerState, mode: Mode) {
    clear(BG);
    draw_text(MARGIN, 20, b"WASMOS TETRIS", 3, TEXT);
    let tag: &[u8] = match mode {
        Mode::Solo => b"SINGLE PLAYER",
        Mode::Host => b"HOST",
        Mode::Join => b"GUEST",
    };
    draw_text(W - MARGIN - text_width(tag.len(), 2), 26, tag, 2, DIM);

    match mode {
        Mode::Solo => {
            draw_text(SOLO_X, HEADER - 30, b"SCORE", 2, DIM);
            draw_number(SOLO_X + BOARD_PX_W, HEADER - 30, me.score, 2, TEXT);
            draw_cells(SOLO_X, HEADER, &me.cells);
            draw_active(SOLO_X, HEADER, me);
            let sx = SOLO_X + BOARD_PX_W + 28;
            draw_text(sx, HEADER + 8, b"LINES", 2, DIM);
            draw_number(sx + text_width(6, 2), HEADER + 40, me.lines, 2, TEXT);
            if me.over {
                banner(b"GAME OVER", b"PRESS Q TO QUIT");
            }
        }
        _ => {
            // Left = you, right = opponent.
            draw_text(LEFT_X, HEADER - 30, b"YOU", 2, TEXT);
            draw_number(LEFT_X + BOARD_PX_W, HEADER - 30, me.score, 2, TEXT);
            draw_cells(LEFT_X, HEADER, &me.cells);
            draw_active(LEFT_X, HEADER, me);

            draw_text(RIGHT_X, HEADER - 30, b"OPPONENT", 2, TEXT);
            draw_number(RIGHT_X + BOARD_PX_W, HEADER - 30, peer.score, 2, TEXT);
            draw_cells(RIGHT_X, HEADER, &peer.cells);

            // Center status column.
            draw_text(CENTER_X, HEADER + 8, b"LINES", 1, DIM);
            draw_number(CENTER_X + text_width(6, 2), HEADER + 34, me.lines, 2, TEXT);
            if !peer.seen {
                draw_text(CENTER_X, HEADER + 96, b"WAIT", 1, DIM);
            }
            if me.over {
                banner(b"GAME OVER", b"PRESS Q TO QUIT");
            } else if peer.over {
                banner(b"YOU WIN", b"PRESS Q TO QUIT");
            }
        }
    }
}

fn banner(text: &[u8], sub: &[u8]) {
    let bw = 460;
    let bh = 140;
    let bx = (W - bw) / 2;
    let by = (H - bh) / 2;
    fill_rect(bx, by, bw, bh, 0xFF10_1820);
    stroke_rect(bx, by, bw, bh, TEXT);
    stroke_rect(bx + 3, by + 3, bw - 6, bh - 6, GRID);
    draw_text(
        bx + (bw - text_width(text.len(), 4)) / 2,
        by + 34,
        text,
        4,
        TEXT,
    );
    draw_text(
        bx + (bw - text_width(sub.len(), 1)) / 2,
        by + 104,
        sub,
        1,
        DIM,
    );
}

struct Button {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
}
impl Button {
    fn hit(&self, px_: i32, py: i32) -> bool {
        px_ >= self.x && py >= self.y && px_ < self.x + self.w && py < self.y + self.h
    }
    fn draw(&self, label: &[u8], hot: bool) {
        let bg = if hot { 0xFF2C3E5_6 } else { PANEL };
        fill_rect(self.x, self.y, self.w, self.h, bg);
        stroke_rect(self.x, self.y, self.w, self.h, TEXT);
        let tw = label.len() as i32 * 12;
        draw_text(
            self.x + (self.w - tw) / 2,
            self.y + self.h / 2 - 7,
            label,
            2,
            TEXT,
        );
    }
}

// ---------------------------------------------------------------------------
// Phases: menu, connecting, playing, game over.
// ---------------------------------------------------------------------------

#[derive(PartialEq)]
enum Phase {
    Menu,
    // Non-blocking handshake in flight (host or join). The main loop advances it
    // from the reply-endpoint doorbell; no separate loop, no blocking.
    Connecting(Mode),
    Playing,
    Done,
}

fn run(proc_ep: i32) -> i32 {
    let mut win = match Window::open(proc_ep) {
        Some(w) => w,
        None => return 1,
    };
    log(b"[tetris] window ready\n");
    // Present an initial frame and take keyboard focus so the game receives keys.
    clear(BG);
    win.present();
    win.focus();

    let bx = (W - 360) / 2;
    let solo_btn = Button {
        x: bx,
        y: 300,
        w: 360,
        h: 64,
    };
    let host_btn = Button {
        x: bx,
        y: 384,
        w: 360,
        h: 64,
    };
    let join_btn = Button {
        x: bx,
        y: 468,
        w: 360,
        h: 64,
    };

    let mut phase = Phase::Menu;
    let mut mode = Mode::Solo;
    let mut me = Board::new(unsafe { sched_ticks() } as u32 ^ 0x9E37_79B9);
    let mut peer = PeerState::new();
    let mut net_sel = -1;
    let mut seq: u8 = 0;
    let mut rx_acc = [0u8; 512];
    let mut rx_len = 0usize;
    let mut pkt = [0u8; PKT_LEN];
    let mut last_gravity = unsafe { sched_ticks() };
    let gravity_ticks = 120; // 250 Hz kernel ticks → ~0.48 s per row

    loop {
        win.pump();
        if win.close {
            break;
        }

        match phase {
            Phase::Menu => {
                clear(BG);
                draw_text((W - text_width(13, 4)) / 2, 130, b"WASMOS TETRIS", 4, TEXT);
                draw_text((W - text_width(13, 1)) / 2, 200, b"CHOOSE A MODE", 1, DIM);
                solo_btn.draw(b"SINGLE PLAYER", solo_btn.hit(win.ptr_x, win.ptr_y));
                host_btn.draw(b"BE HOST", host_btn.hit(win.ptr_x, win.ptr_y));
                join_btn.draw(b"JOIN SESSION", join_btn.hit(win.ptr_x, win.ptr_y));
                draw_text(
                    (W - text_width(29, 1)) / 2,
                    574,
                    b"KEYS  1 SOLO   H HOST   J JOIN",
                    1,
                    DIM,
                );
                draw_text(
                    (W - text_width(30, 1)) / 2,
                    606,
                    b"A D MOVE   W ROTATE   S DOWN",
                    1,
                    DIM,
                );
                draw_text((W - text_width(22, 1)) / 2, 626, b"SPACE HARD DROP", 1, DIM);
                win.present();

                // Choose a mode by button click or keyboard shortcut.
                let mut choice: Option<Mode> = None;
                if win.left_click() {
                    if solo_btn.hit(win.ptr_x, win.ptr_y) {
                        choice = Some(Mode::Solo);
                    } else if host_btn.hit(win.ptr_x, win.ptr_y) {
                        choice = Some(Mode::Host);
                    } else if join_btn.hit(win.ptr_x, win.ptr_y) {
                        choice = Some(Mode::Join);
                    }
                }
                match win.key {
                    b'1' => choice = Some(Mode::Solo),
                    b'h' | b'H' => choice = Some(Mode::Host),
                    b'j' | b'J' => choice = Some(Mode::Join),
                    _ => {}
                }

                if let Some(m) = choice {
                    match m {
                        Mode::Solo => {
                            mode = m;
                            begin_match(
                                &win,
                                &mut me,
                                &mut peer,
                                &mut rx_len,
                                &mut seq,
                                &mut net_sel,
                                &mut last_gravity,
                                m,
                            );
                            phase = Phase::Playing;
                        }
                        // Host/Join arm a non-blocking handshake and hand off to
                        // Phase::Connecting; the match itself begins once the
                        // handshake completes (see the Connecting arm below).
                        Mode::Host => {
                            connect_screen(&mut win, b"WAITING FOR OPPONENT", b"PORT 7000");
                            if unsafe { tnet_host_begin(proc_ep, GAME_PORT) } < 0 {
                                error_screen(&mut win, b"HOST FAILED");
                            } else {
                                phase = Phase::Connecting(m);
                            }
                        }
                        Mode::Join => {
                            connect_screen(&mut win, b"CONNECTING", b"10.0.2.2:7000");
                            if unsafe { tnet_join_begin(proc_ep, PEER_ADDR_V4, GAME_PORT) } < 0 {
                                error_screen(&mut win, b"JOIN FAILED");
                            } else {
                                phase = Phase::Connecting(m);
                            }
                        }
                    }
                }
                if phase == Phase::Menu {
                    unsafe {
                        let _ = ipc_select_wait_timeout(win_menu_sel(&win), 120);
                    }
                }
            }

            // Non-blocking handshake in flight. net-stack answers each step with
            // an IPC doorbell on the reply endpoint; the loop blocks on the select
            // set {event_ep, reply_ep} and steps the state machine on each wake.
            // The "waiting" screen was drawn on entry; win.pump() (loop top) keeps
            // the window closeable throughout.
            Phase::Connecting(m) => match unsafe { tnet_net_advance() } {
                1 => {
                    mode = m;
                    begin_match(
                        &win,
                        &mut me,
                        &mut peer,
                        &mut rx_len,
                        &mut seq,
                        &mut net_sel,
                        &mut last_gravity,
                        m,
                    );
                    phase = Phase::Playing;
                }
                rc if rc < 0 => {
                    error_screen(
                        &mut win,
                        if m == Mode::Host {
                            b"HOST FAILED"
                        } else {
                            b"JOIN FAILED"
                        },
                    );
                    phase = Phase::Menu;
                }
                _ => unsafe {
                    let _ = ipc_select_wait_timeout(conn_sel(&win), 250);
                },
            },

            Phase::Playing => {
                if !me.over {
                    match win.key {
                        b'a' | b'A' => me.move_h(-1),
                        b'd' | b'D' => me.move_h(1),
                        b'w' | b'W' => me.rotate(),
                        b's' | b'S' => {
                            let _ = me.step_down();
                        }
                        b' ' => {
                            let _ = me.hard_drop();
                        }
                        _ => {}
                    }
                }

                if mode != Mode::Solo {
                    poll_net(&mut rx_acc, &mut rx_len, &mut peer, &mut me);
                }

                let now = unsafe { sched_ticks() };
                if !me.over && now.wrapping_sub(last_gravity) >= gravity_ticks {
                    last_gravity = now;
                    let _ = me.step_down();
                }

                if mode != Mode::Solo {
                    encode_packet(&me, seq, &mut pkt);
                    seq = seq.wrapping_add(1);
                    unsafe {
                        let _ = tnet_send(pkt.as_ptr(), PKT_LEN as i32);
                    }
                }

                render_game(&me, &peer, mode);
                win.present();

                let done = if mode == Mode::Solo {
                    me.over
                } else {
                    me.over || peer.over
                };
                if done {
                    phase = Phase::Done;
                }

                unsafe {
                    let _ = ipc_select_wait_timeout(net_sel, 33);
                }
            }

            Phase::Done => {
                render_game(&me, &peer, mode);
                win.present();
                if mode != Mode::Solo {
                    // Keep syncing the final state so the peer learns the result.
                    encode_packet(&me, seq, &mut pkt);
                    seq = seq.wrapping_add(1);
                    unsafe {
                        let _ = tnet_send(pkt.as_ptr(), PKT_LEN as i32);
                    }
                    poll_net(&mut rx_acc, &mut rx_len, &mut peer, &mut me);
                }
                if win.key == b'q' || win.key == b'Q' {
                    break;
                }
                unsafe {
                    let _ = ipc_select_wait_timeout(net_sel, 150);
                }
            }
        }
    }

    unsafe {
        tnet_close();
    }
    log(b"[tetris] exit\n");
    0
}

// A menu-only select set (event endpoint) built lazily and cached.
static mut MENU_SEL: i32 = -1;
fn win_menu_sel(win: &Window) -> i32 {
    unsafe {
        if MENU_SEL < 0 {
            MENU_SEL = ipc_select_create();
            let _ = ipc_select_add(MENU_SEL, win.event_ep);
        }
        MENU_SEL
    }
}

// Handshake select set {event_ep, net reply_ep}, built lazily once the reply
// endpoint exists (after a *_begin call). The loop blocks on this while
// connecting so it wakes on either a compositor event (e.g. window close) or a
// net-stack handshake doorbell. The reply endpoint id is stable across matches,
// so caching is safe.
static mut CONN_SEL: i32 = -1;
fn conn_sel(win: &Window) -> i32 {
    unsafe {
        if CONN_SEL < 0 {
            CONN_SEL = ipc_select_create();
            let _ = ipc_select_add(CONN_SEL, win.event_ep);
            let rep = tnet_reply_ep();
            if rep >= 0 {
                let _ = ipc_select_add(CONN_SEL, rep);
            }
        }
        CONN_SEL
    }
}

#[allow(clippy::too_many_arguments)]
fn begin_match(
    win: &Window,
    me: &mut Board,
    peer: &mut PeerState,
    rx_len: &mut usize,
    seq: &mut u8,
    net_sel: &mut i32,
    last_gravity: &mut i32,
    mode: Mode,
) {
    // Fresh state for the match; the seed differs per side via the tick clock.
    *me = Board::new(unsafe { sched_ticks() } as u32 ^ 0x2545_F491);
    *peer = PeerState::new();
    *rx_len = 0;
    *seq = 0;
    *last_gravity = unsafe { sched_ticks() };
    // The gameplay loop blocks on this select set: always the compositor event
    // endpoint, plus the net reply endpoint (RX doorbells) in the versus modes.
    let sel = unsafe { ipc_select_create() };
    unsafe {
        let _ = ipc_select_add(sel, win.event_ep);
        if mode != Mode::Solo {
            let rep = tnet_reply_ep();
            if rep >= 0 {
                let _ = ipc_select_add(sel, rep);
            }
        }
    }
    *net_sel = sel;
    log(b"[tetris] match start\n");
}

fn poll_net(rx_acc: &mut [u8; 512], rx_len: &mut usize, peer: &mut PeerState, me: &mut Board) {
    let mut tmp = [0u8; 256];
    loop {
        let n = unsafe { tnet_poll(tmp.as_mut_ptr(), 256) };
        if n <= 0 {
            if n == -2 {
                // peer closed; treat as opponent gone (win by default)
                peer.over = true;
            }
            break;
        }
        let n = n as usize;
        // append into the accumulation buffer (bounded)
        let mut i = 0;
        while i < n {
            if *rx_len < rx_acc.len() {
                rx_acc[*rx_len] = tmp[i];
                *rx_len += 1;
            }
            i += 1;
        }
    }
    // consume whole frames, resyncing on the magic byte if needed
    loop {
        if *rx_len < PKT_LEN {
            break;
        }
        if rx_acc[0] != PKT_MAGIC || rx_acc[1] != PKT_VER {
            // drop one byte and retry
            for k in 1..*rx_len {
                rx_acc[k - 1] = rx_acc[k];
            }
            *rx_len -= 1;
            continue;
        }
        let prev_g = peer.garbage_out;
        // decode the leading frame
        let mut frame = [0u8; PKT_LEN];
        frame.copy_from_slice(&rx_acc[..PKT_LEN]);
        // first packet establishes the garbage baseline (no retroactive attack)
        let first = !peer.seen;
        decode_packet(&frame, peer);
        if first {
            peer.last_garbage_seen = peer.garbage_out;
        } else {
            let delta = peer.garbage_out.wrapping_sub(peer.last_garbage_seen);
            if delta > 0 && delta < 40 && !me.over {
                me.pending_garbage += delta as u32;
                peer.last_garbage_seen = peer.garbage_out;
            }
        }
        let _ = prev_g;
        // shift the remainder down
        let rem = *rx_len - PKT_LEN;
        for k in 0..rem {
            rx_acc[k] = rx_acc[k + PKT_LEN];
        }
        *rx_len = rem;
    }
}

fn connect_screen(win: &mut Window, line1: &[u8], line2: &[u8]) {
    clear(BG);
    draw_text((W - 13 * 30) / 2, 80, b"WASMOS TETRIS", 5, TEXT);
    draw_text((W - line1.len() as i32 * 12) / 2, 190, line1, 2, TEXT);
    draw_text((W - line2.len() as i32 * 12) / 2, 230, line2, 2, DIM);
    win.present();
}

fn error_screen(win: &mut Window, msg: &[u8]) {
    clear(BG);
    draw_text((W - msg.len() as i32 * 18) / 2, 180, msg, 3, 0xFFE0524_8);
    draw_text((W - 22 * 6) / 2, 230, b"RETURNING TO MENU", 1, DIM);
    win.present();
    // brief pause so the message is visible
    for _ in 0..40 {
        unsafe {
            let _ = ipc_select_wait_timeout(win_menu_sel(win), 30);
        }
        win.pump();
    }
}

/// The process-manager endpoint is not passed in registers; it lives in the
/// spawn-info buffer (`wasmos_spawn_info_t`, proc_endpoint at byte offset 12).
fn proc_endpoint_from_spawn_info() -> i32 {
    let bid = unsafe { spawn_info_buffer() };
    if bid < 0 {
        return 0;
    }
    let mut hdr = [0u8; 16];
    if unsafe { xfer_buffer_read(bid, hdr.as_mut_ptr() as i32, 16, 0) } != 0 {
        return 0;
    }
    (hdr[12] as u32 | ((hdr[13] as u32) << 8) | ((hdr[14] as u32) << 16) | ((hdr[15] as u32) << 24))
        as i32
}

#[no_mangle]
pub extern "C" fn wasmos_main(_a0: i32, _a1: i32, _a2: i32, _a3: i32) -> i32 {
    let proc_ep = proc_endpoint_from_spawn_info();
    let rc = run(proc_ep);
    unsafe {
        let _ = proc_exit(rc);
    }
    rc
}
