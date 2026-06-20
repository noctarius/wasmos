/// calculator.zig — WASMOS graphical calculator example (Zig).
///
/// Demonstrates the Zig-idiomatic libui wrapper.  Layout uses libui panels
/// for vertical stacking and libui "row" (MENU_BAR) components for the
/// horizontal button grid.  Button colours are driven by bg_color so the
/// updated button renderer respects per-button theming.

const wasmos = @import("wasmos.zig");
const libui = @import("libui.zig");
// ---------------------------------------------------------------------------
// Calculator logic
// ---------------------------------------------------------------------------

/// Actions that a button can trigger.
const Action = enum {
    digit_0, digit_1, digit_2, digit_3, digit_4,
    digit_5, digit_6, digit_7, digit_8, digit_9,
    decimal, negate,
    op_add, op_sub, op_mul, op_div,
    equals, clear, clear_entry, backspace,
};

const Calc = struct {
    const scale: i64 = 1_000_000;
    const frac_digits: usize = 6;

    // Current display string (null-terminated)
    display: [24]u8 = undefined,
    display_len: u8 = 0,
    // Accumulated left-hand value and pending operator
    lhs: i64 = 0,
    op: u8 = 0,   // '+', '-', '*', '/', 0
    // True when the next digit should start a fresh input rather than appending
    fresh: bool = true,
    err: bool = false,

    pub fn init(self: *Calc) void {
        self.display[0] = '0';
        self.display[1] = 0;
        self.display_len = 1;
        self.lhs = 0;
        self.op = 0;
        self.fresh = true;
        self.err = false;
    }

    pub fn displayText(self: *const Calc) [*:0]const u8 {
        return @ptrCast(&self.display);
    }

    fn hasDot(self: *const Calc) bool {
        for (self.display[0..self.display_len]) |c| {
            if (c == '.') return true;
        }
        return false;
    }

    inline fn absI64(v: i64) i64 {
        return if (v < 0) -v else v;
    }

    inline fn currentValue(self: *const Calc) i64 {
        var neg = false;
        var i: usize = 0;
        if (i < self.display_len and self.display[i] == '-') {
            neg = true;
            i += 1;
        }

        var int_part: i64 = 0;
        while (i < self.display_len and self.display[i] >= '0' and self.display[i] <= '9') : (i += 1) {
            int_part = int_part * 10 + @as(i64, self.display[i] - '0');
        }

        var frac_part: i64 = 0;
        if (i < self.display_len and self.display[i] == '.') {
            i += 1;
            var place = scale / 10;
            while (i < self.display_len and self.display[i] >= '0' and self.display[i] <= '9' and place > 0) : (i += 1) {
                frac_part += @as(i64, self.display[i] - '0') * place;
                place = @divTrunc(place, 10);
            }
        }

        const result = int_part * scale + frac_part;
        return if (neg) -result else result;
    }

    inline fn writeUInt(buf: []u8, value: i64) usize {
        if (value == 0) {
            if (buf.len > 0) buf[0] = '0';
            return 1;
        }
        var tmp: [24]u8 = undefined;
        var n: usize = 0;
        var cur = value;
        while (cur > 0 and n < tmp.len) {
            tmp[n] = '0' + @as(u8, @intCast(@rem(cur, 10)));
            n += 1;
            cur = @divTrunc(cur, 10);
        }
        var out: usize = 0;
        while (out < n and out < buf.len) : (out += 1) {
            buf[out] = tmp[n - 1 - out];
        }
        return out;
    }

    inline fn setDisplay(self: *Calc, v: i64) void {
        var pos: usize = 0;
        var mag = v;
        if (v < 0) {
            self.display[pos] = '-';
            pos += 1;
            mag = -v;
        }

        pos += writeUInt(self.display[pos..], @divTrunc(mag, scale));
        const frac = @rem(mag, scale);

        if (frac != 0 and pos < self.display.len) {
            self.display[pos] = '.';
            pos += 1;

            var digits: [frac_digits]u8 = undefined;
            var rem = frac;
            var place = scale / 10;
            var idx: usize = 0;
            while (idx < frac_digits) : (idx += 1) {
                const digit = if (place > 0) @divTrunc(rem, place) else 0;
                digits[idx] = '0' + @as(u8, @intCast(digit));
                if (place > 0) {
                    rem = @rem(rem, place);
                    place = @divTrunc(place, 10);
                }
            }

            var last = frac_digits;
            while (last > 0 and digits[last - 1] == '0') {
                last -= 1;
            }
            for (digits[0..last]) |digit| {
                self.display[pos] = digit;
                pos += 1;
            }
        }

        self.display_len = @intCast(pos);
        self.display[self.display_len] = 0;
    }

    inline fn compute(a: i64, op: u8, b: i64, err: *bool) i64 {
        return switch (op) {
            '+' => a + b,
            '-' => a - b,
            '*' => @divTrunc(a * b, scale),
            '/' => if (b == 0) blk: { err.* = true; break :blk 0; } else @divTrunc(a * scale, b),
            else => b,
        };
    }

    pub fn handle(self: *Calc, action: Action) void {
        if (self.err and action != .clear and action != .clear_entry) return;
        switch (action) {
            .digit_0, .digit_1, .digit_2, .digit_3, .digit_4,
            .digit_5, .digit_6, .digit_7, .digit_8, .digit_9 => {
                const d: u8 = '0' + @as(u8, @intFromEnum(action));
                if (self.fresh) {
                    self.display[0] = d;
                    self.display_len = 1;
                    self.display[1] = 0;
                    self.fresh = false;
                } else {
                    if (self.display_len >= 12) return;
                    // Suppress leading zeros on integer part
                    if (self.display_len == 1 and self.display[0] == '0' and d == '0') return;
                    if (self.display_len == 1 and self.display[0] == '0' and d != '.') {
                        self.display[0] = d;
                    } else {
                        self.display[self.display_len] = d;
                        self.display_len += 1;
                        self.display[self.display_len] = 0;
                    }
                }
            },
            .decimal => {
                if (self.fresh) {
                    self.display[0] = '0'; self.display[1] = '.';
                    self.display_len = 2; self.display[2] = 0;
                    self.fresh = false;
                } else {
                    if (!self.hasDot() and self.display_len < 12) {
                        self.display[self.display_len] = '.';
                        self.display_len += 1;
                        self.display[self.display_len] = 0;
                    }
                }
            },
            .negate => {
                const v = self.currentValue();
                self.setDisplay(-v);
                self.fresh = false;
            },
            .op_add => self.applyOp('+'),
            .op_sub => self.applyOp('-'),
            .op_mul => self.applyOp('*'),
            .op_div => self.applyOp('/'),
            .equals => {
                if (self.op == 0) return;
                const result = compute(self.lhs, self.op, self.currentValue(), &self.err);
                self.setDisplay(result);
                self.lhs = result;
                self.op = 0;
                self.fresh = true;
                if (self.err) {
                    const e = "Error";
                    for (e, 0..) |c, i| self.display[i] = c;
                    self.display_len = @intCast(e.len);
                    self.display[self.display_len] = 0;
                }
            },
            .clear_entry => {
                self.display[0] = '0'; self.display_len = 1; self.display[1] = 0;
                self.fresh = true; self.err = false;
            },
            .clear => {
                self.display[0] = '0'; self.display_len = 1; self.display[1] = 0;
                self.lhs = 0; self.op = 0; self.fresh = true; self.err = false;
            },
            .backspace => {
                if (self.fresh) return;
                if (self.display_len <= 1) {
                    self.display[0] = '0'; self.display_len = 1; self.display[1] = 0;
                } else {
                    self.display_len -= 1;
                    self.display[self.display_len] = 0;
                }
            },
        }
    }

    inline fn applyOp(self: *Calc, new_op: u8) void {
        const cur = self.currentValue();
        if (self.op != 0 and !self.fresh) {
            const result = compute(self.lhs, self.op, cur, &self.err);
            self.setDisplay(result);
            self.lhs = result;
        } else {
            self.lhs = cur;
        }
        self.op = new_op;
        self.fresh = true;
    }
};

// Float ↔ string: delegate to wasmos.fmt (no std.fmt — avoids memory.fill).

// ---------------------------------------------------------------------------
// UI constants — dark theme inspired by Windows Calculator
// ---------------------------------------------------------------------------

const COL_BG       = 0xFF1E1E2E; // window background
const COL_DISPLAY  = 0xFF16161E; // display label background
const COL_TEXT     = 0xFFD0D0E8; // display text
const COL_BTN_NUM  = 0xFF2A2A3E; // digit buttons
const COL_BTN_OP   = 0xFF1C3A5A; // operator buttons (+, -, *, /)
const COL_BTN_EQ   = 0xFF0062A5; // equals button (accent blue)
const COL_BTN_CLR  = 0xFF6B2020; // CE / C (red-tinted)
const COL_BTN_MISC = 0xFF242438; // ±, backspace
const COL_FG       = 0xFFFFFFFF; // button text
const WINDOW_CONTENT_W = 280;
const WINDOW_CONTENT_H = 350;
const WINDOW_CHROME_TITLE_H = 24;
const WINDOW_CHROME_BOTTOM_BORDER = 1;

// ---------------------------------------------------------------------------
// Button layout (5 rows × 4 columns)
// ---------------------------------------------------------------------------

const ButtonDef = struct {
    label: [*:0]const u8,
    bg: u32,
    action: Action,
};

const layout: [5][4]ButtonDef = .{
    .{
        .{ .label = "CE",  .bg = COL_BTN_CLR,  .action = .clear_entry },
        .{ .label = "C",   .bg = COL_BTN_CLR,  .action = .clear       },
        .{ .label = "<-",  .bg = COL_BTN_MISC, .action = .backspace   },
        .{ .label = "/",   .bg = COL_BTN_OP,   .action = .op_div      },
    },
    .{
        .{ .label = "7",   .bg = COL_BTN_NUM,  .action = .digit_7    },
        .{ .label = "8",   .bg = COL_BTN_NUM,  .action = .digit_8    },
        .{ .label = "9",   .bg = COL_BTN_NUM,  .action = .digit_9    },
        .{ .label = "*",   .bg = COL_BTN_OP,   .action = .op_mul     },
    },
    .{
        .{ .label = "4",   .bg = COL_BTN_NUM,  .action = .digit_4    },
        .{ .label = "5",   .bg = COL_BTN_NUM,  .action = .digit_5    },
        .{ .label = "6",   .bg = COL_BTN_NUM,  .action = .digit_6    },
        .{ .label = "-",   .bg = COL_BTN_OP,   .action = .op_sub     },
    },
    .{
        .{ .label = "1",   .bg = COL_BTN_NUM,  .action = .digit_1    },
        .{ .label = "2",   .bg = COL_BTN_NUM,  .action = .digit_2    },
        .{ .label = "3",   .bg = COL_BTN_NUM,  .action = .digit_3    },
        .{ .label = "+",   .bg = COL_BTN_OP,   .action = .op_add     },
    },
    .{
        .{ .label = "+/-", .bg = COL_BTN_MISC, .action = .negate     },
        .{ .label = "0",   .bg = COL_BTN_NUM,  .action = .digit_0    },
        .{ .label = ".",   .bg = COL_BTN_NUM,  .action = .decimal    },
        .{ .label = "=",   .bg = COL_BTN_EQ,   .action = .equals     },
    },
};

// ---------------------------------------------------------------------------
// Global state (kept minimal — must fit in the 24 KB data budget)
// ---------------------------------------------------------------------------

var g_calc: Calc = undefined;
var g_display_id: i32 = -1;
var g_btn_ids: [20]i32 = [_]i32{-1} ** 20;

fn startupSelfTest() bool {
    var probe: Calc = undefined;
    probe.init();
    probe.handle(.digit_8);
    probe.handle(.op_mul);
    probe.handle(.digit_2);
    probe.handle(.equals);
    return probe.display_len == 2 and probe.display[0] == '1' and probe.display[1] == '6';
}

// ---------------------------------------------------------------------------
// Button callback (C calling convention, called by libui event dispatch)
// ---------------------------------------------------------------------------

fn onButtonClick(ctx: *anyopaque, id: i32, user: ?*anyopaque) callconv(.c) void {
    _ = user;
    // Find which button was clicked
    for (g_btn_ids, 0..) |btn_id, i| {
        if (btn_id == id) {
            const row = i / 4;
            const col = i % 4;
            g_calc.handle(layout[row][col].action);
            libui_zig_set_text(ctx, g_display_id, g_calc.displayText());
            libui_zig_mark_dirty(ctx);
            return;
        }
    }
}

// Re-declare the two shim functions used directly in the callback
// (avoids needing to import libui.zig in the callback, which Zig can't do from
// within an anyopaque context — the extern fns are linked directly).
extern fn libui_zig_set_text(ctx: *anyopaque, id: i32, text: [*:0]const u8) callconv(.c) void;
extern fn libui_zig_mark_dirty(ctx: *anyopaque) callconv(.c) void;

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

pub fn main() u8 {
    const proc_ep = wasmos.startup.arg(0);
    _ = wasmos.stdlib.println("[calculator] start", .{}) catch {};

    if (!startupSelfTest()) {
        _ = wasmos.stdlib.println("[calculator] selftest failed", .{}) catch {};
        return 1;
    }

    g_calc.init();

    // Compositor CREATE_WINDOW sizes are outer bounds, not content bounds.
    // Account for the title bar and bottom border so the full 350 px libui
    // layout remains visible inside the window content rect.
    var ui = libui.Context.init(proc_ep, wasmos.ipc.createEndpoint() catch return 1,
                                 WINDOW_CONTENT_W,
                                 WINDOW_CONTENT_H + WINDOW_CHROME_TITLE_H + WINDOW_CHROME_BOTTOM_BORDER) catch return 1;
    ui.setTitle("Calculator");

    const root = ui.rootId();
    ui.style(root, .{ .bg = COL_BG, .pad = 4, .gap = 2 });

    // Display label: full width, tall, right-padded text
    g_display_id = ui.createLabel() catch return 1;
    ui.style(g_display_id, .{ .bg = COL_DISPLAY, .fg = COL_TEXT,
                               .preferred_h = 72, .pad = 12 });
    ui.setText(g_display_id, "0");
    ui.appendChild(root, g_display_id);

    // 5 button rows using MENU_BAR horizontal layout.
    // In MENU_BAR layout, children's preferred_h is reinterpreted as their WIDTH.
    // Row preferred_h (in the parent panel) is the button HEIGHT.
    // Use index-based iteration to avoid Zig copying the [4]ButtonDef row slice
    // by value (which generates a memory.copy WASM instruction that WARP rejects).
    const BTN_W = 64; // button width (as preferred_h in the row)
    const BTN_H = 52; // button height (as preferred_h in the parent panel)

    var row_idx: usize = 0;
    while (row_idx < 5) : (row_idx += 1) {
        const row = ui.createRow() catch return 1;
        ui.style(row, .{ .bg = COL_BG, .preferred_h = BTN_H, .pad = 2, .gap = 2 });
        ui.appendChild(root, row);

        var col_idx: usize = 0;
        while (col_idx < 4) : (col_idx += 1) {
            const btn_def = &layout[row_idx][col_idx];
            const btn = ui.createButton() catch return 1;
            const flat_idx = row_idx * 4 + col_idx;
            g_btn_ids[flat_idx] = btn;
            ui.style(btn, .{ .bg = btn_def.bg, .fg = COL_FG,
                              .preferred_h = BTN_W, .clickable = true });
            ui.setText(btn, btn_def.label);
            ui.setClickCallback(btn, @ptrCast(&onButtonClick), null);
            ui.appendChild(row, btn);
        }
    }

    ui.markDirty();
    ui.drain();
    _ = wasmos.stdlib.println("[calculator] ready", .{}) catch {};

    // Main event loop
    while (!ui.closeRequested()) {
        ui.pollAndDrain();
    }

    ui.deinit();
    return 0;
}
