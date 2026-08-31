//! Host unit tests for the tmpfs namespace and storage core
//! (src/drivers/fs_tmpfs/fs_tmpfs_store.zig).
//!
//! The core depends on nothing in the guest environment except a source of pool
//! memory, which on the guest comes from `memory.grow`. These tests supply that
//! source from a static array, which is the whole of what makes the layout, the
//! chain walking, the path resolution and the namespace rules reachable off
//! target. Nothing here is mocked beyond that one seam.
//!
//! Every case calls `resetStore()` first, so a case cannot decide its
//! neighbour's outcome.
const std = @import("std");
const store = @import("fs_tmpfs_store");

/// Chunks the tests can hand out. Eight is 256 blocks, or 128 KiB -- enough to
/// cross several chunk boundaries, which is where the chain walking is
/// interesting, without making the binary large.
const TEST_CHUNKS: usize = 8;

var g_pool: [TEST_CHUNKS]store.BlockChunk = undefined;
var g_pool_used: usize = 0;

/// The test-side `chunk_source`. Hands out the next static chunk, ZEROED, because
/// the guest's source returns freshly grown pages which the WASM specification
/// guarantees are zero -- a source that returned dirty memory would be testing
/// something the guest never does.
fn testChunk() ?*store.BlockChunk {
    if (g_pool_used >= TEST_CHUNKS) return null;
    const chunk = &g_pool[g_pool_used];
    g_pool_used += 1;
    chunk.* = .{
        .next = [_]u16{0} ** store.BLOCKS_PER_CHUNK,
        .used = [_]bool{false} ** store.BLOCKS_PER_CHUNK,
        .data = [_]u8{0} ** (store.BLOCKS_PER_CHUNK * store.BLOCK),
    };
    return chunk;
}

fn resetStore() void {
    g_pool_used = 0;
    store.chunk_source = &testChunk;
    store.reset();
}

/// Create a file under the root and return its node index.
fn makeFile(name: []const u8) u16 {
    const split = store.splitPath(store.ROOT, name).?;
    return store.nodeAlloc(split.parent, split.name, false).?;
}

// --- the root ----------------------------------------------------------------

test "the root exists, is a directory, and is its own parent" {
    resetStore();
    try std.testing.expect(store.g_nodes[store.ROOT].in_use);
    try std.testing.expect(store.g_nodes[store.ROOT].is_dir);
    try std.testing.expectEqual(store.ROOT, store.g_nodes[store.ROOT].parent);
    try std.testing.expectEqual(@as(?u16, store.ROOT), store.resolve(store.ROOT, "/"));
}

test "the root cannot be created or removed: splitPath refuses to name it" {
    resetStore();
    try std.testing.expectEqual(@as(?store.Split, null), store.splitPath(store.ROOT, "/"));
    try std.testing.expectEqual(@as(?store.Split, null), store.splitPath(store.ROOT, ""));
    try std.testing.expectEqual(@as(?store.Split, null), store.splitPath(store.ROOT, "///"));
}

// --- path resolution ---------------------------------------------------------

test "resolve walks directories and refuses a component that is a file" {
    resetStore();
    const dir = store.nodeAlloc(store.ROOT, "sub", true).?;
    const file = store.nodeAlloc(dir, "leaf", false).?;
    try std.testing.expectEqual(@as(?u16, dir), store.resolve(store.ROOT, "sub"));
    try std.testing.expectEqual(@as(?u16, file), store.resolve(store.ROOT, "sub/leaf"));
    try std.testing.expectEqual(@as(?u16, file), store.resolve(store.ROOT, "/sub/leaf"));
    // A file is not a directory, so nothing resolves through it.
    try std.testing.expectEqual(@as(?u16, null), store.resolve(store.ROOT, "sub/leaf/deeper"));
    try std.testing.expectEqual(@as(?u16, null), store.resolve(store.ROOT, "sub/missing"));
}

test "resolve collapses slashes and honours . and .." {
    resetStore();
    const a = store.nodeAlloc(store.ROOT, "a", true).?;
    const b = store.nodeAlloc(a, "b", true).?;
    try std.testing.expectEqual(@as(?u16, b), store.resolve(store.ROOT, "a///b"));
    try std.testing.expectEqual(@as(?u16, b), store.resolve(store.ROOT, "./a/./b/."));
    try std.testing.expectEqual(@as(?u16, a), store.resolve(store.ROOT, "a/b/.."));
    try std.testing.expectEqual(@as(?u16, b), store.resolve(b, "."));
    // An absolute path restarts at the root whatever the caller stands in.
    try std.testing.expectEqual(@as(?u16, a), store.resolve(b, "/a"));
}

test ".. at the root stays at the root and cannot escape the filesystem" {
    resetStore();
    try std.testing.expectEqual(@as(?u16, store.ROOT), store.resolve(store.ROOT, ".."));
    try std.testing.expectEqual(@as(?u16, store.ROOT), store.resolve(store.ROOT, "../../.."));
    const a = store.nodeAlloc(store.ROOT, "a", true).?;
    try std.testing.expectEqual(@as(?u16, store.ROOT), store.resolve(a, "../../.."));
}

test "names are case sensitive, as in WFS" {
    resetStore();
    _ = store.nodeAlloc(store.ROOT, "Boot", true).?;
    try std.testing.expect(store.resolve(store.ROOT, "Boot") != null);
    try std.testing.expectEqual(@as(?u16, null), store.resolve(store.ROOT, "boot"));
}

test "splitPath yields the parent plus the last component, and refuses . and .." {
    resetStore();
    const dir = store.nodeAlloc(store.ROOT, "sub", true).?;
    const split = store.splitPath(store.ROOT, "sub/new").?;
    try std.testing.expectEqual(dir, split.parent);
    try std.testing.expectEqualStrings("new", split.name);
    // Neither dot entry names a thing to create or remove.
    try std.testing.expectEqual(@as(?store.Split, null), store.splitPath(store.ROOT, "sub/."));
    try std.testing.expectEqual(@as(?store.Split, null), store.splitPath(store.ROOT, "sub/.."));
    // A parent that does not exist is not a place to create anything.
    try std.testing.expectEqual(@as(?store.Split, null), store.splitPath(store.ROOT, "missing/new"));
}

test "a name longer than NAME_MAX is refused rather than truncated" {
    resetStore();
    const long = "x" ** (store.NAME_MAX + 1);
    try std.testing.expectEqual(@as(?store.Split, null), store.splitPath(store.ROOT, long));
    try std.testing.expectEqual(@as(?u16, null), store.nodeAlloc(store.ROOT, long, false));
    // Exactly NAME_MAX still fits.
    const max = "x" ** store.NAME_MAX;
    try std.testing.expect(store.nodeAlloc(store.ROOT, max, false) != null);
}

// --- the namespace -----------------------------------------------------------

test "lookup finds an entry only in its own directory" {
    resetStore();
    const a = store.nodeAlloc(store.ROOT, "a", true).?;
    const b = store.nodeAlloc(store.ROOT, "b", true).?;
    const inside = store.nodeAlloc(a, "shared", false).?;
    _ = store.nodeAlloc(b, "shared", false).?;
    try std.testing.expectEqual(@as(?u16, inside), store.lookup(a, "shared"));
    try std.testing.expect(store.lookup(b, "shared").? != inside);
    try std.testing.expectEqual(@as(?u16, null), store.lookup(store.ROOT, "shared"));
}

test "lookupAny reports whether a directory is empty" {
    resetStore();
    const dir = store.nodeAlloc(store.ROOT, "dir", true).?;
    try std.testing.expectEqual(@as(?u16, null), store.lookupAny(dir));
    const child = store.nodeAlloc(dir, "child", false).?;
    try std.testing.expectEqual(@as(?u16, child), store.lookupAny(dir));
    store.nodeFree(child);
    try std.testing.expectEqual(@as(?u16, null), store.lookupAny(dir));
}

test "isAncestor sees a node itself and every directory above it" {
    resetStore();
    const a = store.nodeAlloc(store.ROOT, "a", true).?;
    const b = store.nodeAlloc(a, "b", true).?;
    const c = store.nodeAlloc(b, "c", true).?;
    try std.testing.expect(store.isAncestor(a, c));
    try std.testing.expect(store.isAncestor(store.ROOT, c));
    try std.testing.expect(store.isAncestor(c, c));
    // Downward is not ancestry: this is the direction a rename must refuse.
    try std.testing.expect(!store.isAncestor(c, a));
}

test "the node table is exhausted rather than overrun" {
    resetStore();
    var made: usize = 0;
    var buf: [8]u8 = undefined;
    while (made < store.MAX_NODES + 4) : (made += 1) {
        const name = std.fmt.bufPrint(&buf, "n{d}", .{made}) catch unreachable;
        if (store.nodeAlloc(store.ROOT, name, false) == null) break;
    }
    // Node 0 is the root and is never handed out, so the table yields one fewer.
    try std.testing.expectEqual(store.MAX_NODES - 1, made);
}

test "nodeIsOpen reports a descriptor held on a node" {
    resetStore();
    const file = makeFile("f");
    try std.testing.expect(!store.nodeIsOpen(file));
    store.g_fds[0] = .{ .in_use = true, .node = file };
    try std.testing.expect(store.nodeIsOpen(file));
    store.g_fds[0] = .{};
    try std.testing.expect(!store.nodeIsOpen(file));
}

// --- blocks and file data ----------------------------------------------------

test "block 0 is never allocated, so 0 can mean no block" {
    resetStore();
    var seen: usize = 0;
    while (seen < store.BLOCKS_PER_CHUNK + 2) : (seen += 1) {
        const b = store.blockAlloc() orelse break;
        try std.testing.expect(b != 0);
    }
    try std.testing.expect(seen > store.BLOCKS_PER_CHUNK);
}

test "the pool grows only when every chunk in hand is full" {
    resetStore();
    try std.testing.expectEqual(@as(usize, 0), store.g_block_chunk_count);
    _ = store.blockAlloc().?;
    try std.testing.expectEqual(@as(usize, 1), store.g_block_chunk_count);
    // The first chunk loses slot 0 to the sentinel, so it yields one fewer.
    var i: usize = 1;
    while (i < store.BLOCKS_PER_CHUNK - 1) : (i += 1) _ = store.blockAlloc().?;
    try std.testing.expectEqual(@as(usize, 1), store.g_block_chunk_count);
    _ = store.blockAlloc().?;
    try std.testing.expectEqual(@as(usize, 2), store.g_block_chunk_count);
}

test "a write is readable back byte for byte across chunk boundaries" {
    resetStore();
    const file = makeFile("data");
    // Three chunks' worth, so the chain crosses two boundaries.
    const total: u32 = @intCast(store.BLOCKS_PER_CHUNK * store.BLOCK * 3 / 2);
    var src: [777]u8 = undefined;
    for (&src, 0..) |*b, k| b.* = @truncate(k *% 7 +% 3);

    var off: u32 = 0;
    while (off < total) {
        const want = @min(src.len, total - off);
        try std.testing.expectEqual(want, store.writeAt(file, off, src[0..want]));
        off += @intCast(want);
    }
    try std.testing.expectEqual(total, store.g_nodes[file].size);

    var dst: [777]u8 = undefined;
    off = 0;
    while (off < total) {
        const want = @min(dst.len, total - off);
        try std.testing.expectEqual(want, store.readAt(file, off, dst[0..want]));
        try std.testing.expectEqualSlices(u8, src[0..want], dst[0..want]);
        off += @intCast(want);
    }
}

test "a read stops at the file size, not at the end of its last block" {
    resetStore();
    const file = makeFile("short");
    const payload = "0123456789";
    try std.testing.expectEqual(@as(u32, 10), store.writeAt(file, 0, payload));

    var dst: [64]u8 = undefined;
    try std.testing.expectEqual(@as(u32, 10), store.readAt(file, 0, dst[0..]));
    try std.testing.expectEqualSlices(u8, payload, dst[0..10]);
    // Reading from the size, and past it, yields nothing rather than block padding.
    try std.testing.expectEqual(@as(u32, 0), store.readAt(file, 10, dst[0..]));
    try std.testing.expectEqual(@as(u32, 0), store.readAt(file, 99, dst[0..]));
    try std.testing.expectEqual(@as(u32, 6), store.readAt(file, 4, dst[0..]));
}

test "an overwrite in place leaves the size alone" {
    resetStore();
    const file = makeFile("f");
    _ = store.writeAt(file, 0, "aaaaaaaa");
    try std.testing.expectEqual(@as(u32, 8), store.g_nodes[file].size);
    _ = store.writeAt(file, 2, "bb");
    try std.testing.expectEqual(@as(u32, 8), store.g_nodes[file].size);
    var dst: [8]u8 = undefined;
    _ = store.readAt(file, 0, dst[0..]);
    try std.testing.expectEqualSlices(u8, "aabbaaaa", dst[0..]);
}

test "a write past the end fills the gap with zeros rather than leaving a hole" {
    resetStore();
    const file = makeFile("sparse");
    const far: u32 = @intCast(store.BLOCK * 3 + 17);
    _ = store.writeAt(file, 0, "head");
    try std.testing.expectEqual(@as(u32, 2), store.writeAt(file, far, "xy"));
    try std.testing.expectEqual(far + 2, store.g_nodes[file].size);

    var dst: [32]u8 = undefined;
    // The blocks the gap spans read as zero, not as whatever was there.
    try std.testing.expectEqual(@as(u32, 32), store.readAt(file, store.BLOCK, dst[0..]));
    for (dst) |b| try std.testing.expectEqual(@as(u8, 0), b);
    var tail: [2]u8 = undefined;
    try std.testing.expectEqual(@as(u32, 2), store.readAt(file, far, tail[0..]));
    try std.testing.expectEqualSlices(u8, "xy", tail[0..]);
}

test "truncate releases the blocks, and the next write reuses them" {
    resetStore();
    const file = makeFile("f");
    const total: u32 = @intCast(store.BLOCKS_PER_CHUNK * store.BLOCK);
    var block: [store.BLOCK]u8 = [_]u8{0xAB} ** store.BLOCK;
    var off: u32 = 0;
    while (off < total) : (off += @intCast(store.BLOCK)) {
        _ = store.writeAt(file, off, block[0..]);
    }
    const grown = store.g_block_chunk_count;
    try std.testing.expect(grown >= 1);

    store.truncateBlocks(file, 0);
    store.g_nodes[file].size = 0;
    try std.testing.expectEqual(@as(u16, 0), store.g_nodes[file].first);

    // Refilling must come out of the released blocks, not out of a bigger pool.
    off = 0;
    while (off < total) : (off += @intCast(store.BLOCK)) {
        _ = store.writeAt(file, off, block[0..]);
    }
    try std.testing.expectEqual(grown, store.g_block_chunk_count);
}

test "a reused block is handed back zeroed, not carrying the old file's bytes" {
    resetStore();
    const first = makeFile("first");
    _ = store.writeAt(first, 0, "SECRET");
    store.truncateBlocks(first, 0);
    store.g_nodes[first].size = 0;

    const second = makeFile("second");
    _ = store.writeAt(second, 4, "z");
    var dst: [5]u8 = undefined;
    try std.testing.expectEqual(@as(u32, 5), store.readAt(second, 0, dst[0..]));
    try std.testing.expectEqualSlices(u8, "\x00\x00\x00\x00z", dst[0..]);
}

test "truncate keeping n blocks drops exactly the tail" {
    resetStore();
    const file = makeFile("f");
    const three: u32 = @intCast(store.BLOCK * 3);
    var pattern: [store.BLOCK]u8 = [_]u8{0x5A} ** store.BLOCK;
    var off: u32 = 0;
    while (off < three) : (off += @intCast(store.BLOCK)) _ = store.writeAt(file, off, pattern[0..]);

    store.truncateBlocks(file, 1);
    store.g_nodes[file].size = @intCast(store.BLOCK);
    var dst: [store.BLOCK]u8 = undefined;
    try std.testing.expectEqual(@as(u32, store.BLOCK), store.readAt(file, 0, dst[0..]));
    try std.testing.expectEqualSlices(u8, pattern[0..], dst[0..]);
    // Nothing remains past the kept block.
    try std.testing.expectEqual(@as(u32, 0), store.readAt(file, store.BLOCK, dst[0..]));
}

test "nodeFree releases the file's blocks with it" {
    resetStore();
    const file = makeFile("f");
    var block: [store.BLOCK]u8 = [_]u8{1} ** store.BLOCK;
    _ = store.writeAt(file, 0, block[0..]);
    const chunks = store.g_block_chunk_count;
    store.nodeFree(file);
    try std.testing.expect(!store.g_nodes[file].in_use);
    // The block came back, so a fresh file of the same size needs no new chunk.
    const again = makeFile("g");
    _ = store.writeAt(again, 0, block[0..]);
    try std.testing.expectEqual(chunks, store.g_block_chunk_count);
}

// --- exhaustion --------------------------------------------------------------

test "a full pool yields a SHORT write rather than a failed one" {
    resetStore();
    const file = makeFile("big");
    // The pool the tests supply is finite, so writing past it must report what
    // was stored -- the shape a full disk gives.
    const capacity: u32 = @intCast(TEST_CHUNKS * store.BLOCKS_PER_CHUNK * store.BLOCK);
    var block: [store.BLOCK]u8 = [_]u8{7} ** store.BLOCK;
    var off: u32 = 0;
    var short: bool = false;
    while (off < capacity + @as(u32, @intCast(store.BLOCK * 4))) : (off += @intCast(store.BLOCK)) {
        const wrote = store.writeAt(file, off, block[0..]);
        if (wrote < store.BLOCK) {
            short = true;
            break;
        }
    }
    try std.testing.expect(short);
    // What was accepted is still readable, so a short write is not a lost file.
    var dst: [store.BLOCK]u8 = undefined;
    try std.testing.expectEqual(@as(u32, store.BLOCK), store.readAt(file, 0, dst[0..]));
    try std.testing.expectEqualSlices(u8, block[0..], dst[0..]);
}

test "with no chunk source nothing is allocated, rather than reaching for memory" {
    resetStore();
    store.chunk_source = null;
    try std.testing.expectEqual(@as(?u16, null), store.blockAlloc());
    const file = makeFile("f");
    try std.testing.expectEqual(@as(u32, 0), store.writeAt(file, 0, "x"));
}
