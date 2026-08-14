import {ipc, startup} from "./wasmos";
import {WASMOS_ERR_NONE} from "./wasmos_status";

const SVC_IPC_LOOKUP_REQ: i32 = 0x221;
const SVC_IPC_LOOKUP_RESP: i32 = 0x2a1;

const GFX_IPC_ABI_MAGIC: i32 = 0x47465850;
const GFX_IPC_ABI_VERSION: i32 = 1;
const GFX_IPC_CREATE_WINDOW: i32 = 0x0200;
const GFX_IPC_ALLOC_SHARED_BUFFER: i32 = 0x0203;
const GFX_IPC_PRESENT_WINDOW: i32 = 0x0205;
const GFX_IPC_PUSH_EVENT: i32 = 0x0206;
const GFX_IPC_RELEASE_SHARED_BUFFER: i32 = 0x0207;
const GFX_IPC_DESTROY_WINDOW: i32 = 0x0201;
const GFX_IPC_SET_WINDOW_TITLE: i32 = 0x020e;

const GFX_EVENT_NONE: i32 = 0;
const GFX_EVENT_POINTER: i32 = 4;
const GFX_EVENT_CLOSE_REQUEST: i32 = 5;
const GFX_EVENT_RESIZE: i32 = 6;

const PAGE_SIZE: i32 = 4096;
const TITLE_BYTES_MAX: i32 = 127;


@external("wasmos", "ipc_endpoint_owner") declare function ipc_endpoint_owner(endpoint: i32): i32;


@external("wasmos", "shmem_create") declare function shmem_create(pages: i32, flags: i32): i32;


@external("wasmos", "shmem_grant") declare function shmem_grant(id: i32, targetPid: i32): i32;


@external("wasmos", "shmem_map_auto") declare function shmem_map_auto(id: i32, size: i32): i32;


@external("wasmos", "shmem_flush") declare function shmem_flush(id: i32, ptr: i32, size: i32): i32;


@external("wasmos", "shmem_unmap") declare function shmem_unmap(id: i32): i32;


@external("wasmos", "sched_yield") declare function sched_yield(): i32;

// Bit masks for the pointer button state carried by GFX_EVENT_POINTER, for use
// with Context.pointerPressed and Context.activate.
export const POINTER_LEFT: u32 = 1;
export const POINTER_RIGHT: u32 = 2;
export const POINTER_MIDDLE: u32 = 4;

// Axis-aligned rectangle in window pixels. Used for hit-testing, not drawing;
// the Surface methods take loose coordinates.
export class Rect {
    constructor(
        public x: i32 = 0,
        public y: i32 = 0,
        public w: i32 = 0,
        public h: i32 = 0,
    ) {}
}

// A clickable region. This binding is immediate-mode: a Button carries only its
// bounds, has no appearance of its own, and does not join any component tree.
// The app draws it each frame and asks Context.activate whether it was clicked.
export class Button {
    bounds: Rect;

    constructor(x: i32 = 0, y: i32 = 0, w: i32 = 0, h: i32 = 0) {
        this.bounds = new Rect(x, y, w, h);
    }

    // Move/resize the region. Typically called every frame from the renderer so
    // the hit area follows the layout it just drew.
    setBounds(x: i32, y: i32, w: i32, h: i32): void {
        this.bounds.x = x;
        this.bounds.y = y;
        this.bounds.w = w;
        this.bounds.h = h;
    }
}

function alignUp(value: i32, alignment: i32): i32 {
    return (value + (alignment - 1)) & ~(alignment - 1);
}

function packVersionOpcode(version: i32, opcode: i32): i32 {
    return (version << 16) | (opcode & 0xffff);
}

function packName16(name: string): StaticArray<i32> {
    const args = new StaticArray<i32>(4);
    const bytes = Uint8Array.wrap(String.UTF8.encode(name, true));
    for (let i = 0; i < 16 && i < bytes.length - 1; i++) {
        const slot = i >> 2;
        const shift = (i & 3) << 3;
        unchecked((args[slot] = unchecked(args[slot]) | ((bytes[i] as i32) << shift)));
    }
    return args;
}

function copyBytes(dst: i32, src: Uint8Array, len: i32): void {
    memory.copy(dst, src.dataStart as usize, len as usize);
}

function zeroMemory(dst: i32, len: i32): void {
    memory.fill(dst, 0, len);
}

// Drawing interface over the window's shared buffer, obtained from
// Context.beginFrame and only meaningful until the matching endFrame. Every
// method writes 0xAARRGGBB pixels directly with no blending, and clips to the
// window; drawing before a buffer is mapped is a silent no-op. The pixels of a
// new frame are whatever the previous frame left, so a full clear is the
// caller's job.
export class Surface {
    constructor(private ctx: Context) {}

    // Overwrite the whole window with a single colour.
    clear(color: u32): void {
        this.fillRect(0, 0, this.ctx.contentWidth(), this.ctx.contentHeight(), color);
    }

    // Fill a rectangle, clipped to the window. A non-positive width or height
    // draws nothing.
    fillRect(x: i32, y: i32, w: i32, h: i32, color: u32): void {
        this.ctx.fillRectInternal(x, y, w, h, color);
    }

    // Draw a `thickness`-pixel border inset inside the rectangle (the edges are
    // painted within it, not around it). A non-positive thickness draws nothing.
    strokeRect(x: i32, y: i32, w: i32, h: i32, thickness: i32, color: u32): void {
        this.ctx.strokeRectInternal(x, y, w, h, thickness, color);
    }

    // Fill a disc of `radius` pixels centred on (cx, cy). Unantialiased, and a
    // non-positive radius draws nothing.
    fillCircle(cx: i32, cy: i32, radius: i32, color: u32): void {
        this.ctx.fillCircleInternal(cx, cy, radius, color);
    }

    // Draw one digit from the built-in 3x5 bitmap font with its top-left at
    // (x, y), each font pixel expanded to `scale` screen pixels. Only 0-8 are
    // available; see drawDigit3x5Internal.
    drawDigit3x5(x: i32, y: i32, digit: i32, scale: i32, color: u32): void {
        this.ctx.drawDigit3x5Internal(x, y, digit, scale, color);
    }
}

// One compositor window plus its shared framebuffer and input state.
//
// This is a self-contained immediate-mode client, not a binding to the C libui:
// it speaks the gfx IPC protocol directly and offers no components, layout,
// text rendering or event callbacks. A frame is beginFrame -> draw -> endFrame,
// and input is polled with pump/pointerPressed rather than dispatched.
export class Context {
    private procEndpoint: i32 = -1;
    private gfxEndpoint: i32 = -1;
    // Endpoint the compositor pushes GFX_IPC_PUSH_EVENT to; it is the window's
    // owner (source of CREATE_WINDOW). pump() blocks on it, so an idle UI sleeps
    // rather than polling. Requests use ipc.call's private reply endpoint;
    // compositor ownership is per-process so that separation is allowed.
    private eventEndpoint: i32 = -1;
    private gfxOwnerPid: i32 = -1;
    private windowId: i32 = -1;
    private width: i32 = 0;
    private height: i32 = 0;
    private strideBytes: i32 = 0;
    private bufferId: i32 = 0;
    private shmemId: i32 = -1;
    private mappedPtr: i32 = 0;
    private mappedLen: i32 = 0;
    private titleShmemId: i32 = -1;
    private titlePtr: i32 = 0;
    private closeRequestedFlag: bool = false;
    private pointerXValue: i32 = 0;
    private pointerYValue: i32 = 0;
    private pointerButtonsValue: u32 = 0;
    private previousButtons: u32 = 0;
    private surface: Surface = new Surface(this);

    // Create a `width` x `height` window titled `title`, returning null on any
    // failure. Takes the process-manager endpoint from startup argument 0, so
    // it only works in a spawned WASMOS app, and blocks while it looks the gfx
    // service up (retrying up to 4096 times with sched_yield before giving up).
    // A returned Context owns a window, a shared buffer and two shmem mappings
    // that destroy() releases.
    //
    // TODO: Once the AssemblyScript build grows a real Wasm link step, replace
    // the transport-backed implementation here with the shared C libui shim ABI.
    static open(width: i32, height: i32, title: string): Context | null {
        const procEndpoint = startup.arg(0);
        if (procEndpoint <= 0 || width <= 0 || height <= 0) {
            return null;
        }

        const ctx = new Context();
        ctx.procEndpoint = procEndpoint;
        ctx.gfxEndpoint = ctx.lookupService("gfx");
        if (ctx.gfxEndpoint <= 0) {
            return null;
        }
        ctx.gfxOwnerPid = ipc_endpoint_owner(ctx.gfxEndpoint);
        if (ctx.gfxOwnerPid <= 0) {
            return null;
        }
        ctx.eventEndpoint = ipc.createEndpoint();
        if (ctx.eventEndpoint < 0) {
            return null;
        }

        // Create the window sourced from the event endpoint so it becomes the
        // window owner and receives pushed events; block for the reply there.
        if (
            !ipc.reply(
                ctx.gfxEndpoint,
                ctx.eventEndpoint,
                GFX_IPC_CREATE_WINDOW,
                1,
                width,
                height,
                GFX_IPC_ABI_MAGIC,
                packVersionOpcode(GFX_IPC_ABI_VERSION, GFX_IPC_CREATE_WINDOW),
            )
        ) {
            return null;
        }
        const create = ipc.recv(ctx.eventEndpoint);
        if (create == null || create.type != 0x0280 || create.arg0 != WASMOS_ERR_NONE) {
            return null;
        }

        ctx.windowId = create.arg1;
        ctx.width = width;
        ctx.height = height;
        if (
            !ctx.ensureTitleBuffer() ||
            !ctx.setTitle(title) ||
            !ctx.allocateBuffer(width, height)
        ) {
            ctx.destroy();
            return null;
        }
        return ctx;
    }

    // Release the window, the shared buffer and both shmem mappings, and reset
    // the ids so a second call is harmless. Drawing after this does nothing.
    destroy(): void {
        if (this.titleShmemId > 0 && this.titlePtr != 0) {
            let _ = shmem_unmap(this.titleShmemId);
        }
        this.titleShmemId = -1;
        this.titlePtr = 0;
        if (this.windowId > 0 && this.gfxEndpoint > 0) {
            let _ = ipc.call(this.gfxEndpoint, GFX_IPC_DESTROY_WINDOW, this.windowId, 0, 0, 0);
        }
        if (this.bufferId != 0 && this.gfxEndpoint > 0) {
            // Release after destroying the window: only then does the compositor
            // stop considering the buffer busy and reclaim the slot.
            let _ = ipc.call(
                this.gfxEndpoint,
                GFX_IPC_RELEASE_SHARED_BUFFER,
                this.bufferId,
                0,
                0,
                0,
            );
        }
        if (this.shmemId > 0 && this.mappedPtr != 0) {
            let _ = shmem_unmap(this.shmemId);
        }
        this.mappedPtr = 0;
        this.mappedLen = 0;
        this.bufferId = 0;
        this.shmemId = -1;
        this.windowId = -1;
    }

    // True once the compositor has asked this window to close. Only updated by
    // pump, so it changes between iterations of the app's own loop.
    shouldClose(): bool {
        return this.closeRequestedFlag;
    }

    // Current window size in pixels. It changes when pump handles a resize
    // event, so a layout computed from it is only valid for the current frame.
    contentWidth(): i32 {
        return this.width;
    }

    contentHeight(): i32 {
        return this.height;
    }

    // Last pointer position seen for this window, in window pixels. Retains the
    // last value while the pointer is elsewhere; it is not clamped or reset.
    pointerX(): i32 {
        return this.pointerXValue;
    }

    pointerY(): i32 {
        return this.pointerYValue;
    }

    // Start a frame. Returns the context's single Surface — the same object
    // every time, not a new buffer — so drawing is always into the currently
    // mapped shared buffer.
    beginFrame(): Surface {
        return this.surface;
    }

    // Flush the shared buffer and present it. Returns true when the compositor
    // acknowledged the present, false when the window is not fully set up, the
    // flush failed, or the compositor refused (which also happens when the
    // window resized underneath this frame).
    //
    // The flush is required: it is what makes the pixels written through the
    // mapping visible to the compositor.
    endFrame(): bool {
        if (
            this.shmemId <= 0 ||
            this.mappedPtr == 0 ||
            this.gfxEndpoint <= 0 ||
            this.windowId <= 0 ||
            this.bufferId == 0
        ) {
            return false;
        }
        const byteLen = this.strideBytes * this.height;
        if (shmem_flush(this.shmemId, this.mappedPtr, byteLen) != 0) {
            return false;
        }
        const reply = ipc.call(
            this.gfxEndpoint,
            GFX_IPC_PRESENT_WINDOW,
            this.windowId,
            this.bufferId,
            0,
            0,
        );
        return reply != null && reply.type == 0x0280 && reply.arg0 == WASMOS_ERR_NONE;
    }

    // Block for one pushed event, process it, and return 1 if it was handled
    // (0 for a non-event message or GFX_EVENT_NONE). The caller loops, so the
    // thread sleeps in the kernel between events instead of polling. `limit` is
    // ignored: exactly one event is consumed per call.
    pump(limit: i32 = 8): i32 {
        this.previousButtons = this.pointerButtonsValue;
        let handled = 0;
        {
            const reply = ipc.recv(this.eventEndpoint);
            if (reply == null || reply.type != GFX_IPC_PUSH_EVENT) {
                return 0;
            }
            const eventType = reply.arg1;
            if (eventType == GFX_EVENT_NONE) {
                return 0;
            }
            handled++;
            if (eventType == GFX_EVENT_POINTER) {
                if (reply.arg2 == this.windowId) {
                    this.pointerXValue = reply.arg3 & 0xfff;
                    this.pointerYValue = (reply.arg3 >>> 12) & 0xfff;
                    this.pointerButtonsValue = ((reply.arg3 >>> 24) & 0xff) as u32;
                }
            } else if (eventType == GFX_EVENT_CLOSE_REQUEST && reply.arg2 == this.windowId) {
                this.closeRequestedFlag = true;
            } else if (eventType == GFX_EVENT_RESIZE && reply.arg2 == this.windowId) {
                const newWidth = reply.arg3 & 0xffff;
                const newHeight = (reply.arg3 >>> 16) & 0xffff;
                if (newWidth > 0 && newHeight > 0) {
                    this.resizeTo(newWidth, newHeight);
                }
            }
        }
        return handled;
    }

    // Set the window title, returning true when the compositor acknowledged it.
    // The string is UTF-8 encoded into a shmem page granted to the compositor
    // and truncated to 127 bytes — by byte, so it can split a multi-byte
    // sequence. Blocks for one compositor round trip.
    setTitle(title: string): bool {
        if (!this.ensureTitleBuffer()) {
            return false;
        }
        const bytes = Uint8Array.wrap(String.UTF8.encode(title, false));
        let len = bytes.length;
        if (len <= 0) {
            len = 1;
            store<u8>(this.titlePtr, 0);
        } else {
            if (len > TITLE_BYTES_MAX) {
                len = TITLE_BYTES_MAX;
            }
            copyBytes(this.titlePtr, bytes, len);
            store<u8>(this.titlePtr + len, 0);
        }
        if (shmem_flush(this.titleShmemId, this.titlePtr, len + 1) != 0) {
            return false;
        }
        const reply = ipc.call(
            this.gfxEndpoint,
            GFX_IPC_SET_WINDOW_TITLE,
            this.windowId,
            this.titleShmemId,
            len,
            0,
        );
        return reply != null && reply.type == 0x0280 && reply.arg0 == WASMOS_ERR_NONE;
    }

    // True when the last known pointer position lies inside `rect`, with the
    // left/top edges inside and the right/bottom edges outside. Says nothing
    // about button state.
    hitTest(rect: Rect): bool {
        return (
            this.pointerXValue >= rect.x &&
            this.pointerYValue >= rect.y &&
            this.pointerXValue < rect.x + rect.w &&
            this.pointerYValue < rect.y + rect.h
        );
    }

    // True on the frame where `button` is clicked: a press edge for `mask`
    // combined with the pointer being inside the button's bounds. Call it once
    // per frame after pump — the edge stays true until the next pump.
    activate(button: Button, mask: u32 = POINTER_LEFT): bool {
        return this.pointerPressed(mask) && this.hitTest(button.bounds);
    }

    // True when any button in `mask` is down now and was up at the previous
    // pump, i.e. a press edge rather than a held button. Releases are not
    // reported.
    pointerPressed(mask: u32): bool {
        return (this.pointerButtonsValue & mask) != 0 && (this.previousButtons & mask) == 0;
    }

    // Yield the CPU to the scheduler. pump already blocks until an event
    // arrives, so this is only needed by a loop that also does work between
    // frames and wants to be a good citizen.
    yield(): void {
        let _ = sched_yield();
    }

    // Implementation behind Surface.fillRect. Public only because Surface holds
    // the Context by reference rather than the framebuffer; prefer the Surface
    // methods. Clips to the window and no-ops without a mapped buffer.
    fillRectInternal(x: i32, y: i32, w: i32, h: i32, color: u32): void {
        if (this.mappedPtr == 0 || w <= 0 || h <= 0) {
            return;
        }
        let x0 = x < 0 ? 0 : x;
        let y0 = y < 0 ? 0 : y;
        let x1 = x + w;
        let y1 = y + h;
        if (x1 > this.width) x1 = this.width;
        if (y1 > this.height) y1 = this.height;
        if (x0 >= x1 || y0 >= y1) {
            return;
        }
        for (let yy = y0; yy < y1; yy++) {
            const row = this.mappedPtr + yy * this.strideBytes;
            for (let xx = x0; xx < x1; xx++) {
                store<u32>(row + (xx << 2), color);
            }
        }
    }

    // Implementation behind Surface.strokeRect: four inset edge fills. A
    // thickness above half the rectangle overdraws the middle.
    strokeRectInternal(x: i32, y: i32, w: i32, h: i32, thickness: i32, color: u32): void {
        if (thickness <= 0 || w <= 0 || h <= 0) {
            return;
        }
        this.fillRectInternal(x, y, w, thickness, color);
        this.fillRectInternal(x, y + h - thickness, w, thickness, color);
        this.fillRectInternal(x, y, thickness, h, color);
        this.fillRectInternal(x + w - thickness, y, thickness, h, color);
    }

    // Implementation behind Surface.fillCircle: a hard-edged distance test over
    // the bounding box, so the edge is aliased and cost is O(radius^2).
    fillCircleInternal(cx: i32, cy: i32, radius: i32, color: u32): void {
        if (radius <= 0 || this.mappedPtr == 0) {
            return;
        }
        const r2 = radius * radius;
        for (let dy = -radius; dy <= radius; dy++) {
            const yy = cy + dy;
            if (yy < 0 || yy >= this.height) continue;
            for (let dx = -radius; dx <= radius; dx++) {
                if (dx * dx + dy * dy > r2) continue;
                const xx = cx + dx;
                if (xx < 0 || xx >= this.width) continue;
                store<u32>(this.mappedPtr + yy * this.strideBytes + (xx << 2), color);
            }
        }
    }

    // Draws digits 0-8 from a 3x5 bitmap font, scaled by `scale`.
    // FIXME: the glyph table has no '9', so drawDigit3x5(9) draws nothing.
    drawDigit3x5Internal(x: i32, y: i32, digit: i32, scale: i32, color: u32): void {
        if (digit < 0 || digit > 8 || scale <= 0) {
            return;
        }
        const glyphs = StaticArray.fromArray<i32>([
            0b111101101101111, 0b010110010010111, 0b111001111100111, 0b111001111001111,
            0b101101111001001, 0b111100111001111, 0b111100111101111, 0b111001001001001,
            0b111101111101111,
        ]);
        const bits = unchecked(glyphs[digit]);
        for (let row = 0; row < 5; row++) {
            for (let col = 0; col < 3; col++) {
                const bit = 14 - (row * 3 + col);
                if (((bits >> bit) & 1) != 0) {
                    this.fillRectInternal(x + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
    }

    private lookupService(name: string): i32 {
        const packed = packName16(name);
        for (let attempt = 0; attempt < 4096; attempt++) {
            const reply = ipc.call(
                this.procEndpoint,
                SVC_IPC_LOOKUP_REQ,
                unchecked(packed[0]),
                unchecked(packed[1]),
                unchecked(packed[2]),
                unchecked(packed[3]),
            );
            if (reply != null && reply.type == SVC_IPC_LOOKUP_RESP && reply.arg0 != -1) {
                return reply.arg0;
            }
            let _ = sched_yield();
        }
        return -1;
    }

    private ensureTitleBuffer(): bool {
        if (this.titleShmemId > 0 && this.titlePtr != 0) {
            return true;
        }
        this.titleShmemId = shmem_create(1, 0);
        if (this.titleShmemId <= 0) {
            return false;
        }
        if (shmem_grant(this.titleShmemId, this.gfxOwnerPid) != 0) {
            return false;
        }
        this.titlePtr = shmem_map_auto(this.titleShmemId, PAGE_SIZE);
        if (this.titlePtr <= 0) {
            this.titlePtr = 0;
            return false;
        }
        zeroMemory(this.titlePtr, PAGE_SIZE);
        return true;
    }

    private resizeTo(width: i32, height: i32): bool {
        if (width == this.width && height == this.height) {
            return true;
        }
        this.width = width;
        this.height = height;
        return this.allocateBuffer(width, height);
    }

    private allocateBuffer(width: i32, height: i32): bool {
        if (this.shmemId > 0) {
            if (this.mappedPtr != 0) {
                let _ = shmem_unmap(this.shmemId);
            }
            if (this.bufferId != 0) {
                let _ = ipc.call(
                    this.gfxEndpoint,
                    GFX_IPC_RELEASE_SHARED_BUFFER,
                    this.bufferId,
                    0,
                    0,
                    0,
                );
            }
            this.bufferId = 0;
            this.shmemId = -1;
            this.mappedPtr = 0;
            this.mappedLen = 0;
        }

        const reply = ipc.call(
            this.gfxEndpoint,
            GFX_IPC_ALLOC_SHARED_BUFFER,
            this.windowId,
            width,
            height,
            0,
        );
        if (reply == null || reply.type != 0x0280 || reply.arg0 != WASMOS_ERR_NONE) {
            return false;
        }
        this.bufferId = reply.arg1;
        this.shmemId = reply.arg2;
        this.strideBytes = reply.arg3;
        const byteLen = this.strideBytes * height;
        this.mappedLen = alignUp(byteLen, PAGE_SIZE);
        this.mappedPtr = shmem_map_auto(this.shmemId, this.mappedLen);
        if (this.mappedPtr <= 0) {
            this.mappedPtr = 0;
            return false;
        }
        return true;
    }
}
