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

/// C-compatible button-click callback type.
pub const ClickCallback = *const fn (ctx: *anyopaque, id: i32, user: ?*anyopaque) callconv(.c) void;

/// Style parameters passed to Context.style().
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

/// Wraps the opaque C ui_context_t*.
pub const Context = struct {
    handle: *anyopaque,

    /// Allocate and initialise a libui window of the given pixel dimensions.
    pub fn init(proc_ep: i32, reply_ep: i32, width: i32, height: i32) Error!Context {
        const h = libui_zig_alloc_ctx() orelse return Error.InitFailed;
        if (libui_zig_ui_init(h, proc_ep, reply_ep, width, height) != 0) {
            return Error.InitFailed;
        }
        return Context{ .handle = h };
    }

    pub fn deinit(self: *Context) void {
        libui_zig_ui_destroy(self.handle);
    }

    pub fn setTitle(self: *Context, title: [*:0]const u8) void {
        libui_zig_set_title(self.handle, title);
    }

    pub fn closeRequested(self: *const Context) bool {
        return libui_zig_close_requested(self.handle) != 0;
    }

    pub fn markDirty(self: *Context) void {
        libui_zig_mark_dirty(self.handle);
    }

    pub fn drain(self: *Context) void {
        _ = libui_zig_drain(self.handle);
    }

    /// Poll one GFX event, dispatch it through libui, then layout+render if dirty.
    pub fn pollAndDrain(self: *Context) void {
        libui_zig_poll_and_drain(self.handle);
    }

    pub fn rootId(self: *const Context) i32 {
        return libui_zig_root_id(self.handle);
    }

    // ---- Component creation ------------------------------------------------

    pub fn createPanel(self: *Context) Error!i32 {
        const id = libui_zig_create_panel(self.handle);
        return if (id > 0) id else Error.CreateFailed;
    }

    pub fn createLabel(self: *Context) Error!i32 {
        const id = libui_zig_create_label(self.handle);
        return if (id > 0) id else Error.CreateFailed;
    }

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

    pub fn appendChild(self: *Context, parent_id: i32, child_id: i32) void {
        libui_zig_append_child(self.handle, parent_id, child_id);
    }

    pub fn setText(self: *Context, id: i32, text: [*:0]const u8) void {
        libui_zig_set_text(self.handle, id, text);
    }

    pub fn setClickCallback(self: *Context, id: i32, cb: ClickCallback, user: ?*anyopaque) void {
        libui_zig_set_button_action(self.handle, id, @ptrCast(cb), user);
    }

    /// Apply a Style to a component.  Zero-valued fields leave the component's
    /// existing value unchanged (use explicit 0 colors only when intentional).
    pub fn style(self: *Context, id: i32, s: Style) void {
        libui_zig_set_bg_color(self.handle, id, s.bg);
        libui_zig_set_fg_color(self.handle, id, s.fg);
        if (s.border != 0 or s.border_px > 0) {
            libui_zig_set_border_color(self.handle, id, s.border);
        }
        if (s.preferred_h != 0) libui_zig_set_preferred_h(self.handle, id, s.preferred_h);
        if (s.pad != 0)          libui_zig_set_padding_px(self.handle, id, s.pad);
        if (s.gap != 0)          libui_zig_set_gap_px(self.handle, id, s.gap);
        if (s.border_px != 0)    libui_zig_set_border_px(self.handle, id, s.border_px);
        if (s.clickable)         libui_zig_set_clickable(self.handle, id, 1);
    }
};
