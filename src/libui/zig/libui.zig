/// libui.zig — Zig-idiomatic wrapper for the WASMOS libui C library.
///
/// The actual libui functions are static-inline C; they are compiled into the
/// WASM module via libui_shim.c and exposed here as extern C declarations
/// (no @cImport needed).  The Context type owns an opaque pointer to the C
/// ui_context_t allocated by the shim's arena allocator.
///
/// Usage:
///   var ui = try libui.Context.init(proc_ep, reply_ep, 280, 350);
///   defer ui.deinit();
///   try ui.setTitle("Calculator");
///   const panel = try ui.createPanel();
///   ui.style(panel, .{ .bg = 0xFF1E1E2E, .pad = 4, .gap = 2 });
///   ui.markDirty();
///   while (!ui.closeRequested()) {
///       ui.pollAndDrain();
///   }
/// InitFailed covers both the arena allocation of the context and the window
/// bring-up itself; CreateFailed means a component could not be allocated.
/// Neither carries a status code — the underlying C API only reports -1.
const Error = error{ InitFailed, CreateFailed };

// ---------------------------------------------------------------------------
// Shim extern declarations
// ---------------------------------------------------------------------------

extern fn libui_zig_alloc_ctx() callconv(.c) ?*anyopaque;
extern fn libui_zig_ui_init(ctx: *anyopaque, proc_ep: i32, reply_ep: i32, w: i32, h: i32) callconv(.c) i32;
extern fn libui_zig_ui_destroy(ctx: *anyopaque) callconv(.c) void;
extern fn libui_zig_set_title(ctx: *anyopaque, title: [*:0]const u8) callconv(.c) void;
extern fn libui_zig_close_requested(ctx: *const anyopaque) callconv(.c) i32;
extern fn libui_zig_mark_dirty(ctx: *anyopaque) callconv(.c) void;
extern fn libui_zig_drain(ctx: *anyopaque) callconv(.c) i32;
extern fn libui_zig_poll_and_drain(ctx: *anyopaque) callconv(.c) void;
extern fn libui_zig_root_id(ctx: *const anyopaque) callconv(.c) i32;

extern fn libui_zig_create_panel(ctx: *anyopaque) callconv(.c) i32;
extern fn libui_zig_create_label(ctx: *anyopaque) callconv(.c) i32;
extern fn libui_zig_create_button(ctx: *anyopaque) callconv(.c) i32;
extern fn libui_zig_create_menu_bar(ctx: *anyopaque) callconv(.c) i32;

extern fn libui_zig_append_child(ctx: *anyopaque, parent_id: i32, child_id: i32) callconv(.c) void;
extern fn libui_zig_set_text(ctx: *anyopaque, id: i32, text: [*:0]const u8) callconv(.c) void;
extern fn libui_zig_set_button_action(ctx: *anyopaque, id: i32, cb: *const anyopaque, user: ?*anyopaque) callconv(.c) void;

extern fn libui_zig_set_bg_color(ctx: *anyopaque, id: i32, color: u32) callconv(.c) void;
extern fn libui_zig_set_fg_color(ctx: *anyopaque, id: i32, color: u32) callconv(.c) void;
extern fn libui_zig_set_border_color(ctx: *anyopaque, id: i32, color: u32) callconv(.c) void;
extern fn libui_zig_set_preferred_h(ctx: *anyopaque, id: i32, h: i32) callconv(.c) void;
extern fn libui_zig_set_padding_px(ctx: *anyopaque, id: i32, px: i32) callconv(.c) void;
extern fn libui_zig_set_gap_px(ctx: *anyopaque, id: i32, px: i32) callconv(.c) void;
extern fn libui_zig_set_border_px(ctx: *anyopaque, id: i32, px: i32) callconv(.c) void;
extern fn libui_zig_set_clickable(ctx: *anyopaque, id: i32, val: i32) callconv(.c) void;

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

/// C-compatible button-click callback type. `ctx` is the same opaque context
/// pointer the Context wraps, so a callback can drive the shim's extern
/// functions directly (see calculator.zig). `id` is the component that fired
/// and `user` the pointer registered with it, which libui never dereferences.
/// The callback runs inside pollAndDrain, on the app's own thread.
pub const ClickCallback = *const fn (ctx: *anyopaque, id: i32, user: ?*anyopaque) callconv(.c) void;

/// Style parameters passed to Context.style(). Colours are 0xAARRGGBB.
/// `preferred_h` is a height under a panel and a WIDTH under a row; `pad` is
/// the inset on all four sides, `gap` the spacing between children, and
/// `border_px` the border width. Note the defaults are not "leave unchanged":
/// see Context.style() for which fields are written unconditionally.
pub const Style = struct {
    bg: u32 = 0xFF202833,
    fg: u32 = 0xFFFFFFFF,
    border: u32 = 0x00000000,
    preferred_h: i32 = 0,
    pad: i32 = 0,
    gap: i32 = 0,
    border_px: i32 = 0,
    clickable: bool = false,
};

/// Wraps the opaque C ui_context_t*. The pointer is arena-allocated and lives
/// for the rest of the process, so a Context may be copied freely; deinit
/// releases the window and the component tree, not the allocation.
pub const Context = struct {
    handle: *anyopaque,

    /// Allocate and initialise a libui window of the given pixel dimensions.
    ///
    /// `proc_ep` is the process-manager endpoint (startup argument 0) and
    /// `reply_ep` an endpoint the caller owns, used for libui's synchronous
    /// compositor and font requests; the Context does not take ownership of it.
    /// Blocks while it resolves the gfx and font services, which it retries with
    /// sched_yield rather than waiting indefinitely. Returns InitFailed on any
    /// failure, with nothing left to clean up.
    pub fn init(proc_ep: i32, reply_ep: i32, width: i32, height: i32) Error!Context {
        const h = libui_zig_alloc_ctx() orelse return Error.InitFailed;
        if (libui_zig_ui_init(h, proc_ep, reply_ep, width, height) != 0) {
            return Error.InitFailed;
        }
        return Context{ .handle = h };
    }

    /// Destroy the window and free the component tree, leaving the underlying
    /// context zeroed. Safe to call twice; the handle itself stays valid but
    /// must not be used for anything else afterwards.
    pub fn deinit(self: *Context) void {
        libui_zig_ui_destroy(self.handle);
    }

    /// Set the window title. Silently ignored when the title is empty or longer
    /// than 47 bytes — the underlying call refuses rather than truncating, and
    /// the failure is not surfaced here.
    pub fn setTitle(self: *Context, title: [*:0]const u8) void {
        libui_zig_set_title(self.handle, title);
    }

    /// True once the compositor has asked this window to close. The flag is
    /// only updated by event dispatch, so it changes across pollAndDrain.
    pub fn closeRequested(self: *const Context) bool {
        return libui_zig_close_requested(self.handle) != 0;
    }

    /// Mark the tree dirty so the next drain repaints. Required after any
    /// setText or style change made outside libui's own event handling.
    pub fn markDirty(self: *Context) void {
        libui_zig_mark_dirty(self.handle);
    }

    /// Lay out, render and present if the tree is dirty; a no-op otherwise.
    /// Blocks for the compositor round trips a repaint needs. The failure
    /// result is discarded.
    pub fn drain(self: *Context) void {
        _ = libui_zig_drain(self.handle);
    }

    /// Block until the compositor pushes one GFX event, dispatch it through
    /// libui, then lay out and render if anything became dirty. An idle UI
    /// sleeps in the kernel here rather than spinning.
    ///
    /// One event per call, so this belongs in a `while (!closeRequested())`
    /// loop; once close has been requested it stops waiting and only drains.
    /// Application click callbacks run from inside this call.
    pub fn pollAndDrain(self: *Context) void {
        libui_zig_poll_and_drain(self.handle);
    }

    /// Component id of the root panel, which init sized to the window. Parent
    /// for the app's own top-level components.
    pub fn rootId(self: *const Context) i32 {
        return libui_zig_root_id(self.handle);
    }

    // ---- Component creation ------------------------------------------------
    // Each returns the new component's id, or CreateFailed when the arena is
    // exhausted. The component starts detached: appendChild puts it in the tree.

    /// Vertical container: children are stacked top to bottom at their
    /// preferred_h, separated by the panel's gap.
    pub fn createPanel(self: *Context) Error!i32 {
        const id = libui_zig_create_panel(self.handle);
        return if (id > 0) id else Error.CreateFailed;
    }

    /// Left-aligned, vertically centred text in the component's fg colour.
    pub fn createLabel(self: *Context) Error!i32 {
        const id = libui_zig_create_label(self.handle);
        return if (id > 0) id else Error.CreateFailed;
    }

    /// Centred text with a pressed state. Needs `clickable` in its Style, or a
    /// setClickCallback, before it responds to a click.
    pub fn createButton(self: *Context) Error!i32 {
        const id = libui_zig_create_button(self.handle);
        return if (id > 0) id else Error.CreateFailed;
    }

    /// A menu-bar component uses horizontal layout: children are placed side
    /// by side and their preferred_h is interpreted as preferred WIDTH.
    pub fn createRow(self: *Context) Error!i32 {
        const id = libui_zig_create_menu_bar(self.handle);
        return if (id > 0) id else Error.CreateFailed;
    }

    // ---- Component manipulation --------------------------------------------

    /// Append `child_id` as the last child of `parent_id`. Failures (an unknown
    /// id, or appending a component to itself) are silently ignored.
    pub fn appendChild(self: *Context, parent_id: i32, child_id: i32) void {
        libui_zig_append_child(self.handle, parent_id, child_id);
    }

    /// Copy `text` into the component. libui stores its own copy, so `text`
    /// only has to live for the duration of the call. Does not repaint by
    /// itself — pair it with markDirty when changing a live component.
    pub fn setText(self: *Context, id: i32, text: [*:0]const u8) void {
        libui_zig_set_text(self.handle, id, text);
    }

    /// Install `cb` and make the component clickable. `user` is stored as an
    /// opaque pointer and passed back on every call; it must outlive the
    /// context, since libui neither copies nor frees it.
    pub fn setClickCallback(self: *Context, id: i32, cb: ClickCallback, user: ?*anyopaque) void {
        libui_zig_set_button_action(self.handle, id, @ptrCast(cb), user);
    }

    /// Apply a Style to a component. bg and fg are always written, so a Style
    /// with defaulted colours resets them to 0xFF202833 / 0xFFFFFFFF. The
    /// remaining fields are written only when non-zero (clickable only when
    /// true), so a partially filled Style keeps the component's current
    /// geometry — a border colour needs either border or border_px set.
    pub fn style(self: *Context, id: i32, s: Style) void {
        libui_zig_set_bg_color(self.handle, id, s.bg);
        libui_zig_set_fg_color(self.handle, id, s.fg);
        if (s.border != 0 or s.border_px > 0) {
            libui_zig_set_border_color(self.handle, id, s.border);
        }
        if (s.preferred_h != 0) libui_zig_set_preferred_h(self.handle, id, s.preferred_h);
        if (s.pad != 0) libui_zig_set_padding_px(self.handle, id, s.pad);
        if (s.gap != 0) libui_zig_set_gap_px(self.handle, id, s.gap);
        if (s.border_px != 0) libui_zig_set_border_px(self.handle, id, s.border_px);
        if (s.clickable) libui_zig_set_clickable(self.handle, id, 1);
    }
};
