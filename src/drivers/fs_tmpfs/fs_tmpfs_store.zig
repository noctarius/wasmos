//! fs_tmpfs_store.zig — the namespace and storage core of the in-memory
//! filesystem, with no dependency on the guest environment.
//!
//! Split out of `fs_tmpfs.zig` so it can be linked and exercised on the HOST
//! (`tests/unit/test_fs_tmpfs_store.zig`). Everything here is decided by the
//! tables below and its arguments: no IPC, no transfer buffers, no console, and
//! no allocator. The one thing it cannot do for itself is obtain memory for the
//! block pool, which on the guest comes from `memory.grow`; that is the
//! `chunk_source` seam, and it is the whole of what the two environments differ
//! by.
//!
//! Indices, never pointers
//! -----------------------
//! A node and a block are named by INDEX throughout, and an index stays valid for
//! the life of the process: the node table is fixed, and a pool chunk is never
//! moved and never freed. That is what lets file chains, open descriptors and a
//! client's working directory all hold bare integers.
//!
//! Zero-initialized state
//! ----------------------
//! Every static here is zero-initialized, which is load-bearing on the guest: a
//! Zig WASM module's INITIALIZED data must stay under the layout budget
//! `scripts/wasm_stack_check.py` enforces, so the sentinels are chosen to be
//! zero. Block 0 is never allocated, which is what lets `0` mean "no block" in
//! both `Node.first` and a chunk's `next`; the root is node 0 and is its own
//! parent, which is what makes `..` at the root stay at the root.

/// Longest single path component, matching WFS_NAME_MAX (wfs_format.h) and FAT's
/// long-name limit. Parity is the point: a filename valid on another mount must be
/// creatable here, or copying a file between mounts fails on the name alone.
///
/// The name lives in the node record, so this is 255 bytes per node whether the
/// names are long or not -- 50 KiB of table rather than 13 KiB. That is the price
/// of the parity and is paid per instance; variable-length names in a cell arena
/// would not pay it, at the cost of an allocator for them (docs/TASKS.md).
///
/// 255 is also the ceiling `name_len` can express as a u8, so raising it further
/// is not just a constant.
pub const NAME_MAX: usize = 255;
/// Files plus directories, across the whole filesystem.
pub const MAX_NODES: usize = 192;
/// Bytes per storage block. A file wastes at most one short block.
pub const BLOCK: usize = 512;
/// Blocks per pool chunk. A chunk is allocated whole and never moved or freed,
/// which is what keeps a block INDEX stable.
pub const BLOCKS_PER_CHUNK: usize = 64;
/// Chunks the block index space can address, and therefore this filesystem's
/// ceiling: MAX_BLOCK_CHUNKS * BLOCKS_PER_CHUNK * BLOCK, or 8 MiB. It is a
/// ceiling and not a cost -- an unallocated chunk is one null pointer.
pub const MAX_BLOCK_CHUNKS: usize = 256;
/// Open files across all clients.
pub const MAX_FDS: usize = 32;
/// First descriptor handed out; 0..2 stay clear of the standard streams, as in
/// every other backend.
pub const FD_BASE: i32 = 3;
/// The root, which always exists and is its own parent.
pub const ROOT: u16 = 0;

/// One file or directory. `parent` of the root is the root, so `..` there stays
/// put; `first` is 0 for a directory and for an empty file, since block 0 is
/// never allocated.
pub const Node = struct {
    in_use: bool = false,
    is_dir: bool = false,
    name_len: u8 = 0,
    parent: u16 = 0,
    first: u16 = 0,
    size: u32 = 0,
    name: [NAME_MAX]u8 = [_]u8{0} ** NAME_MAX,
};

/// One open file. `node` is meaningful only while `in_use`.
pub const Fd = struct {
    in_use: bool = false,
    node: u16 = 0,
    flags: i32 = 0,
    offset: u32 = 0,
};

/// One chunk of the block pool: the blocks themselves plus the chain and
/// allocation metadata for exactly those blocks, so growing the pool grows its
/// bookkeeping with it rather than leaving a separate array to outgrow.
pub const BlockChunk = struct {
    /// Next block in each chain, 0 terminating it.
    next: [BLOCKS_PER_CHUNK]u16,
    /// Whether each block belongs to some chain.
    used: [BLOCKS_PER_CHUNK]bool,
    data: [BLOCKS_PER_CHUNK * BLOCK]u8,
};

pub var g_nodes: [MAX_NODES]Node = [_]Node{.{}} ** MAX_NODES;
pub var g_fds: [MAX_FDS]Fd = [_]Fd{.{}} ** MAX_FDS;

/// The pool, filled a chunk at a time on demand. A null entry is a chunk that has
/// never been needed and costs one pointer, which is what keeps an instance that
/// stores nothing cheap -- the mount is per-instance and a system may run several.
pub var g_block_chunks: [MAX_BLOCK_CHUNKS]?*BlockChunk = [_]?*BlockChunk{null} ** MAX_BLOCK_CHUNKS;
pub var g_block_chunk_count: usize = 0;

/// Where pool chunks come from. The core allocates nothing itself: the guest
/// points this at an arena over `memory.grow`, and the host tests point it at a
/// static array. Null means no pool can be grown, so every allocation fails
/// rather than reaching for memory nobody granted.
pub var chunk_source: ?*const fn () ?*BlockChunk = null;

/// Return the filesystem to "just this filesystem's root, and nothing in it".
///
/// The guest calls this once at bring-up; a host test calls it between cases, so
/// one case cannot decide another's outcome. Chunks already handed out by
/// `chunk_source` are dropped rather than returned to it, since the seam has no
/// free operation -- a test's pool is reset by resetting its own source.
pub fn reset() void {
    g_nodes = [_]Node{.{}} ** MAX_NODES;
    g_fds = [_]Fd{.{}} ** MAX_FDS;
    g_block_chunks = [_]?*BlockChunk{null} ** MAX_BLOCK_CHUNKS;
    g_block_chunk_count = 0;
    g_nodes[ROOT] = .{ .in_use = true, .is_dir = true, .parent = ROOT };
}

// --- names -------------------------------------------------------------------

/// A node's name as a slice of its own storage.
pub fn nameOf(n: u16) []const u8 {
    const node = &g_nodes[n];
    return node.name[0..node.name_len];
}

pub fn nameEqual(a: []const u8, b: []const u8) bool {
    if (a.len != b.len) return false;
    for (a, b) |x, y| {
        if (x != y) return false;
    }
    return true;
}

/// The entry named `name` in directory `dir`.
pub fn lookup(dir: u16, name: []const u8) ?u16 {
    var i: u16 = 0;
    while (i < MAX_NODES) : (i += 1) {
        const node = &g_nodes[i];
        if (!node.in_use or node.parent != dir or i == ROOT) continue;
        if (nameEqual(nameOf(i), name)) return i;
    }
    return null;
}

/// Any entry of `dir`, or null when it holds none.
pub fn lookupAny(dir: u16) ?u16 {
    var i: u16 = 0;
    while (i < MAX_NODES) : (i += 1) {
        if (g_nodes[i].in_use and i != ROOT and g_nodes[i].parent == dir) return i;
    }
    return null;
}

/// Whether `maybe_ancestor` is `n` or lies above it. Used to refuse renaming a
/// directory into its own subtree, which would detach it from the root.
pub fn isAncestor(maybe_ancestor: u16, n: u16) bool {
    var cur = n;
    while (true) {
        if (cur == maybe_ancestor) return true;
        if (cur == ROOT) return false;
        cur = g_nodes[cur].parent;
    }
}

// --- blocks ------------------------------------------------------------------

/// The chunk holding `index`, or null when that chunk has not been allocated.
pub fn blockChunkOf(index: u16) ?*BlockChunk {
    const c = @as(usize, index) / BLOCKS_PER_CHUNK;
    if (c >= MAX_BLOCK_CHUNKS) return null;
    return g_block_chunks[c];
}

pub fn blockSlot(index: u16) usize {
    return @as(usize, index) % BLOCKS_PER_CHUNK;
}

/// Next block in `index`'s chain; 0 both terminates a chain and answers for a
/// block whose chunk does not exist, which a caller treats the same way.
pub fn blockNext(index: u16) u16 {
    const chunk = blockChunkOf(index) orelse return 0;
    return chunk.next[blockSlot(index)];
}

pub fn setBlockNext(index: u16, value: u16) void {
    const chunk = blockChunkOf(index) orelse return;
    chunk.next[blockSlot(index)] = value;
}

pub fn releaseBlock(index: u16) void {
    const chunk = blockChunkOf(index) orelse return;
    chunk.used[blockSlot(index)] = false;
    chunk.next[blockSlot(index)] = 0;
}

/// The `BLOCK` bytes of `index`, or null when its chunk does not exist.
pub fn blockBytes(index: u16) ?[]u8 {
    const chunk = blockChunkOf(index) orelse return null;
    const off = blockSlot(index) * BLOCK;
    return chunk.data[off .. off + BLOCK];
}

/// Claim a free block, zeroed, growing the pool by one chunk when every chunk in
/// hand is full. Block 0 is never claimed, so that 0 can mean "none".
pub fn blockAlloc() ?u16 {
    var c: usize = 0;
    while (c < g_block_chunk_count) : (c += 1) {
        const chunk = g_block_chunks[c] orelse continue;
        var i: usize = 0;
        while (i < BLOCKS_PER_CHUNK) : (i += 1) {
            const index = c * BLOCKS_PER_CHUNK + i;
            if (index == 0 or chunk.used[i]) continue;
            chunk.used[i] = true;
            chunk.next[i] = 0;
            @memset(chunk.data[i * BLOCK .. (i + 1) * BLOCK], 0);
            return @intCast(index);
        }
    }
    if (g_block_chunk_count >= MAX_BLOCK_CHUNKS) return null;
    const acquire = chunk_source orelse return null;
    const fresh = acquire() orelse return null;
    const c_new = g_block_chunk_count;
    g_block_chunks[c_new] = fresh;
    g_block_chunk_count += 1;
    // Index 0 is the sentinel, so the very first chunk hands out slot 1. The
    // chunk's bytes came from grown memory and are already zero.
    const slot: usize = if (c_new == 0) 1 else 0;
    fresh.used[slot] = true;
    fresh.next[slot] = 0;
    return @intCast(c_new * BLOCKS_PER_CHUNK + slot);
}

/// Index of the `want`-th block of `n`'s chain, or null when the chain is
/// shorter than that.
pub fn blockAt(n: u16, want: usize) ?u16 {
    var block = g_nodes[n].first;
    var i: usize = 0;
    while (block != 0) {
        if (i == want) return block;
        i += 1;
        block = blockNext(block);
    }
    return null;
}

/// The block holding byte `off` of `n`, appending blocks as far as needed.
/// Returns null when the pool is full, having kept the chain consistent.
pub fn blockForWrite(n: u16, off: u32) ?u16 {
    const want: usize = @as(usize, off) / BLOCK;
    if (blockAt(n, want)) |block| return block;

    // Append from the chain's tail rather than from its head, so a sparse write
    // past the end fills the gap with zeroed blocks instead of leaving a hole
    // this layout cannot express.
    var tail: u16 = 0;
    var have: usize = 0;
    var block = g_nodes[n].first;
    while (block != 0) {
        tail = block;
        have += 1;
        block = blockNext(block);
    }
    while (have <= want) {
        const fresh = blockAlloc() orelse return null;
        if (tail == 0) {
            g_nodes[n].first = fresh;
        } else {
            setBlockNext(tail, fresh);
        }
        tail = fresh;
        have += 1;
    }
    return tail;
}

/// Release every block of `n` from block index `keep` onward.
pub fn truncateBlocks(n: u16, keep: usize) void {
    var prev: u16 = 0;
    var block = g_nodes[n].first;
    var i: usize = 0;
    while (block != 0 and i < keep) {
        prev = block;
        block = blockNext(block);
        i += 1;
    }
    if (prev == 0) {
        g_nodes[n].first = 0;
    } else {
        setBlockNext(prev, 0);
    }
    while (block != 0) {
        const following = blockNext(block);
        releaseBlock(block);
        block = following;
    }
}

/// Copy out of `n` at `off` into `dst`, stopping at the file's size. Returns how
/// many bytes were copied.
pub fn readAt(n: u16, off: u32, dst: []u8) u32 {
    const size = g_nodes[n].size;
    if (off >= size) return 0;
    var want: u32 = @intCast(dst.len);
    if (want > size - off) want = size - off;

    var done: u32 = 0;
    while (done < want) {
        const at = off + done;
        const block = blockAt(n, @as(usize, at) / BLOCK) orelse break;
        const within: usize = @as(usize, at) % BLOCK;
        var chunk: usize = BLOCK - within;
        if (chunk > want - done) chunk = want - done;
        const bytes = blockBytes(block) orelse break;
        @memcpy(dst[done .. done + chunk], bytes[within .. within + chunk]);
        done += @intCast(chunk);
    }
    return done;
}

/// Copy `src` into `n` at `off`, growing the file as needed. Returns how many
/// bytes were stored, which is SHORT when the pool ran out -- the same shape a
/// full disk gives, so a caller reports what it wrote rather than failing whole.
pub fn writeAt(n: u16, off: u32, src: []const u8) u32 {
    var done: u32 = 0;
    while (done < src.len) {
        const at = off + done;
        const block = blockForWrite(n, at) orelse break;
        const within: usize = @as(usize, at) % BLOCK;
        var chunk: usize = BLOCK - within;
        if (chunk > src.len - done) chunk = src.len - done;
        const bytes = blockBytes(block) orelse break;
        @memcpy(bytes[within .. within + chunk], src[done .. done + chunk]);
        done += @intCast(chunk);
        if (at + chunk > g_nodes[n].size) g_nodes[n].size = at + @as(u32, @intCast(chunk));
    }
    return done;
}

// --- nodes -------------------------------------------------------------------

/// Create an entry named `name` in `parent`. The caller has already established
/// that no such entry exists.
pub fn nodeAlloc(parent: u16, name: []const u8, is_dir: bool) ?u16 {
    if (name.len == 0 or name.len > NAME_MAX) return null;
    var i: u16 = 1; // node 0 is the root and is never handed out
    while (i < MAX_NODES) : (i += 1) {
        if (g_nodes[i].in_use) continue;
        g_nodes[i] = .{
            .in_use = true,
            .is_dir = is_dir,
            .name_len = @intCast(name.len),
            .parent = parent,
        };
        @memcpy(g_nodes[i].name[0..name.len], name);
        return i;
    }
    return null;
}

/// Release `n` and everything it stored. The caller has established that it is
/// empty (a directory) and unopened.
pub fn nodeFree(n: u16) void {
    truncateBlocks(n, 0);
    g_nodes[n] = .{};
}

/// Whether any descriptor names `n`. A node cannot be removed or renamed over
/// while one does: a descriptor holds the node index, so freeing it under an open
/// file would leave that descriptor addressing a reused node.
pub fn nodeIsOpen(n: u16) bool {
    for (&g_fds) |*fd| {
        if (fd.in_use and fd.node == n) return true;
    }
    return false;
}

// --- paths -------------------------------------------------------------------

/// Resolve `path` from `cwd` to the node it names.
///
/// An absolute path restarts at the root, `.` keeps the current node, `..` moves
/// to the parent and cannot leave the root, and repeated slashes collapse.
/// Returns null when a component does not exist or when a non-final component is
/// not a directory.
pub fn resolve(cwd: u16, path: []const u8) ?u16 {
    var cur: u16 = if (path.len > 0 and path[0] == '/') ROOT else cwd;
    var i: usize = 0;
    while (i < path.len) {
        if (path[i] == '/') {
            i += 1;
            continue;
        }
        const start = i;
        while (i < path.len and path[i] != '/') i += 1;
        const seg = path[start..i];
        if (seg.len == 1 and seg[0] == '.') continue;
        if (seg.len == 2 and seg[0] == '.' and seg[1] == '.') {
            cur = g_nodes[cur].parent;
            continue;
        }
        if (!g_nodes[cur].is_dir) return null;
        cur = lookup(cur, seg) orelse return null;
    }
    return cur;
}

/// A path's last component and the directory that holds it.
pub const Split = struct { parent: u16, name: []const u8 };

/// Split `path` into the directory holding its last component and that
/// component.
///
/// What every create and remove needs: the parent must exist and be a directory,
/// while the component itself may or may not. Returns null when the parent does
/// not resolve, when the path names no component at all (the root), when the last
/// component is `.` or `..` -- neither of which names a thing to create or remove
/// -- or when the name does not fit a node.
pub fn splitPath(cwd: u16, path: []const u8) ?Split {
    var last_start: usize = 0;
    var last_end: usize = 0;
    var i: usize = 0;
    while (i < path.len) {
        if (path[i] == '/') {
            i += 1;
            continue;
        }
        last_start = i;
        while (i < path.len and path[i] != '/') i += 1;
        last_end = i;
    }
    if (last_end == last_start) return null;
    const name = path[last_start..last_end];
    if (name.len > NAME_MAX) return null;
    if (nameEqual(name, ".") or nameEqual(name, "..")) return null;

    const parent = resolve(cwd, path[0..last_start]) orelse return null;
    if (!g_nodes[parent].is_dir) return null;
    return .{ .parent = parent, .name = name };
}
