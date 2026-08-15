const FS_IPC_OPEN_REQ: i32 = 0x400;
const FS_IPC_READ_REQ: i32 = 0x401;
const FS_IPC_WRITE_REQ: i32 = 0x406;
const FS_IPC_CLOSE_REQ: i32 = 0x402;
const FS_IPC_STAT_REQ: i32 = 0x403;
const FS_IPC_SEEK_REQ: i32 = 0x405;
const FS_IPC_UNLINK_REQ: i32 = 0x407;
const FS_IPC_MKDIR_REQ: i32 = 0x408;
const FS_IPC_RMDIR_REQ: i32 = 0x409;
const FS_IPC_READDIR_REQ: i32 = 0x410;
const FS_IPC_RESP: i32 = 0x480;
const FS_IPC_STREAM: i32 = 0x481;

const IPC_FIELD_TYPE: i32 = 0;
const IPC_FIELD_REQUEST_ID: i32 = 1;
const IPC_FIELD_ARG0: i32 = 2;
const IPC_FIELD_ARG1: i32 = 3;
const IPC_FIELD_SOURCE: i32 = 4;
const IPC_FIELD_DESTINATION: i32 = 5;
const IPC_FIELD_ARG2: i32 = 6;
const IPC_FIELD_ARG3: i32 = 7;

/*
 * SEEK_SET / SEEK_CUR / SEEK_END are the whence values for File.seek: the new
 * offset is measured from the start of the file, from the current offset, or
 * from the file size. The FS backend refuses a resulting offset outside
 * [0, size], so seeking past the end fails instead of extending the file.
 *
 * S_IFREG and S_IFDIR are the file-type bits of FileStat.mode; fs.stat masks off
 * everything else, and the FS reply carries no permission bits.
 *
 * O_RDONLY..O_TRUNC are POSIX-valued open flags. Bit 0 is the access mode
 * (O_RDONLY or O_WRONLY); the rest are modifiers the FS backend accepts only
 * alongside O_WRONLY. There is no read/write mode: any other bit is rejected.
 */
export const SEEK_SET: i32 = 0;
export const SEEK_CUR: i32 = 1;
export const SEEK_END: i32 = 2;
export const S_IFREG: i32 = 0x8000;
export const S_IFDIR: i32 = 0x4000;
export const O_RDONLY: i32 = 0;
export const O_WRONLY: i32 = 1;
export const O_APPEND: i32 = 0x0008;
export const O_CREAT: i32 = 0x0040;
export const O_TRUNC: i32 = 0x0200;


@external("wasmos", "console_write") declare function console_write(ptr: i32, len: i32): i32;


@external("wasmos", "console_read") declare function console_read(ptr: i32, len: i32): i32;


@external("wasmos", "proc_exit") declare function proc_exit(status: i32): i32;


@external("wasmos", "ipc_create_endpoint") declare function ipc_create_endpoint(): i32;


@external("wasmos", "ipc_send")
declare function ipc_send(
    destination_endpoint: i32,
    source_endpoint: i32,
    type: i32,
    request_id: i32,
    arg0: i32,
    arg1: i32,
    arg2: i32,
    arg3: i32,
): i32;


@external("wasmos", "ipc_recv") declare function ipc_recv(endpoint: i32): i32;


@external("wasmos", "ipc_last_field") declare function ipc_last_field(field: i32): i32;


@external("wasmos", "fs_endpoint") declare function fs_endpoint(): i32;


@external("wasmos", "xfer_buffer_size") declare function xfer_buffer_size(): i32;
// Object/owner/borrow xfer ABI (owner-push): read/write name the object by
// buffer_id. acquire creates an owned buffer; borrow grants a named endpoint's
// context rights over it; release destroys it. See src/libc/src/unistd.c.
@external("wasmos", "xfer_buffer_write")
declare function xfer_buffer_write(bufferId: i32, ptr: i32, len: i32, offset: i32): i32;


@external("wasmos", "xfer_buffer_read")
declare function xfer_buffer_read(bufferId: i32, ptr: i32, len: i32, offset: i32): i32;


@external("wasmos", "xfer_buffer_acquire")
declare function xfer_buffer_acquire(minimumSize: i32): i32;


@external("wasmos", "xfer_buffer_borrow")
declare function xfer_buffer_borrow(granteeEndpoint: i32, bufferId: i32, flags: i32): i32;


@external("wasmos", "xfer_buffer_release") declare function xfer_buffer_release(bufferId: i32): i32;
// GRANT flags mirror WASMOS_BUFFER_GRANT_READ|WRITE.
const XFER_GRANT_RW: i32 = 0x3;


@external("wasmos", "spawn_info_buffer") declare function spawn_info_buffer(): i32;


@external("wasmos", "thread_gettid") declare function thread_gettid(): i32;


@external("wasmos", "thread_yield") declare function thread_yield(): i32;


@external("wasmos", "mutex_try_lock") declare function mutex_try_lock(ptr: i32): i32;


@external("wasmos", "mutex_unlock") declare function mutex_unlock(ptr: i32): i32;

let g_fsReplyEndpoint: i32 = -1;
let g_fsRequestId: i32 = 1;
let g_ipcReplyEndpoint: i32 = -1;
let g_ipcRequestId: i32 = 1;
let g_startupArgs = new StaticArray<i32>(4);

// Startup contract (mirrors wasmos_spawn_info_t in wasmos_spawn_info.h).
const SPAWN_INFO_MAGIC: u32 = 0x57535049; // 'WSPI'
let g_spawnValid: bool = false;
let g_spawnLoaded: bool = false;
let g_spawnProcEndpoint: i32 = 0;
let g_spawnTty: i32 = 0;
let g_spawnModuleCount: u32 = 0;
let g_spawnModuleIndex: u32 = 0;
let g_spawnArgsOff: u32 = 0;
let g_spawnArgsLen: u32 = 0;

// Lazy + idempotent: works for both wasmos_main apps and initialize-entry
// services/drivers (which never call runMain).
function loadSpawnInfo(): void {
    if (g_spawnLoaded) return;
    g_spawnLoaded = true;
    g_spawnValid = false;
    const bid = spawn_info_buffer();
    if (bid <= 0) return;
    const hdr = new Uint8Array(36);
    if (xfer_buffer_read(bid, hdr.dataStart as i32, 36, 0) != 0) return;
    if (load<u32>(hdr.dataStart) != SPAWN_INFO_MAGIC) return;
    g_spawnProcEndpoint = load<i32>(hdr.dataStart, 12);
    g_spawnTty = load<i32>(hdr.dataStart, 16);
    g_spawnModuleCount = load<u32>(hdr.dataStart, 20);
    g_spawnModuleIndex = load<u32>(hdr.dataStart, 24);
    g_spawnArgsOff = load<u32>(hdr.dataStart, 28);
    g_spawnArgsLen = load<u32>(hdr.dataStart, 32);
    g_spawnValid = true;
}

/**
 * Values the process manager handed this process at spawn time, read from the
 * spawn-info buffer. Every accessor but `arg` loads that buffer on demand, so
 * they work in a service or driver entered through `initialize` as well as in an
 * app that went through runMain; all of them report 0 when the process has no
 * spawn info.
 */
export namespace startup {
    // Legacy accessor: index 0 == proc.endpoint (from spawn-info); 1..3 == 0.
    // Unlike the other accessors this one reads a cache that only runMain fills,
    // so it stays 0 in a service or driver that never calls runMain.
    export function arg(index: i32): i32 {
        if (index < 0 || index >= 4) {
            return 0;
        }
        return unchecked(g_startupArgs[index]);
    }
    /** IPC endpoint of the process manager, for spawn/exit protocol requests. */
    export function procEndpoint(): i32 {
        loadSpawnInfo();
        return g_spawnProcEndpoint;
    }
    /**
     * Id of the controlling TTY the process manager allocated, 0 when none was
     * allocated.
     */
    export function tty(): i32 {
        loadSpawnInfo();
        return g_spawnTty;
    }
    /** Number of boot modules in this process's boot list. */
    export function moduleCount(): u32 {
        loadSpawnInfo();
        return g_spawnModuleCount;
    }
    /** This module's index in that boot list, 0 when not applicable. */
    export function moduleIndex(): u32 {
        loadSpawnInfo();
        return g_spawnModuleIndex;
    }
}


@unmanaged
/**
 * Recursive mutex whose state lives in guest memory and whose arbitration is
 * done by the kernel, laid out like wasmos_mutex_t: `owner_tid` is the thread id
 * of the current owner (0 when unlocked) and `recursion_depth` the number of
 * unmatched acquisitions it holds. Both are written by the kernel, which is why
 * the class is unmanaged -- its object address is passed to the host call.
 *
 * The kernel link tables export no `wasmos.mutex_try_lock` / `mutex_unlock`
 * (FIXME(user-mutex-import) in src/libc/include/wasmos/api.h), so a module that
 * actually calls these fails to instantiate on an unresolved import.
 */
export class Mutex {
    owner_tid: u32;
    recursion_depth: u32;

    /**
     * Resets to the unlocked state. Zeroing a mutex another thread holds loses
     * that ownership, so only init one nobody has locked.
     */
    init(): void {
        this.owner_tid = 0;
        this.recursion_depth = 0;
    }

    /** The calling thread's id, as the kernel records it in `owner_tid`. */
    static currentTid(): i32 {
        return thread_gettid();
    }

    /**
     * One acquisition attempt, never blocking: 0 when the mutex is now held by
     * this thread (raising `recursion_depth` if it already was), 1 when another
     * thread owns it, negative on error.
     */
    tryLock(): i32 {
        return mutex_try_lock(changetype<i32>(this));
    }

    /**
     * Acquires the mutex, yielding the thread between attempts while another
     * owner holds it. Returns 0 once held, or the negative code that ended the
     * retry loop. A yield-spin, not a sleep.
     */
    lock(): i32 {
        while (true) {
            const rc = this.tryLock();
            if (rc != 1) {
                return rc;
            }
            thread_yield();
        }
        return -1;
    }

    /**
     * Drops one acquisition, releasing the mutex when `recursion_depth` reaches
     * zero. Returns 0 on success, negative when the caller is not the owner.
     */
    unlock(): i32 {
        return mutex_unlock(changetype<i32>(this));
    }
}

function readSpawnArgs(): Array<string> {
    // Argv is the args blob in the spawn-info buffer (loaded by loadSpawnInfo).
    if (!g_spawnValid || g_spawnArgsLen == 0) {
        return new Array<string>();
    }
    const bid = spawn_info_buffer();
    if (bid <= 0) {
        return new Array<string>();
    }
    let n: i32 = <i32>g_spawnArgsLen;
    if (n > 127) n = 127;
    const buf = new Uint8Array(n + 1);
    if (xfer_buffer_read(bid, buf.dataStart as i32, n, <i32>g_spawnArgsOff) != 0) {
        return new Array<string>();
    }
    if (n == 0) {
        return new Array<string>();
    }
    const raw = String.UTF8.decodeUnsafe(buf.dataStart, n, false);
    const parts = raw.split(" ");
    const result = new Array<string>();
    for (let i = 0; i < parts.length; i++) {
        const token = unchecked(parts[i]);
        if (token.length > 0) {
            result.push(token);
        }
    }
    return result;
}

/**
 * Application entry shim: loads the spawn info, hands `entry` the argv parsed
 * from the spawn-info args blob, and reports its return value to the process
 * manager through proc_exit.
 *
 * The four entry-arg registers are ignored -- the process manager passes zeros
 * in them -- and only `startup.arg(0)` is populated, with the process manager
 * endpoint. Argv is split on spaces with no quoting, and the blob is truncated
 * to 127 bytes. proc_exit does not return, so the trailing return is unreachable
 * in a live process.
 */
export function runMain(entry: (args: Array<string>) => i32): i32 {
    // Every startup value comes from the spawn-info buffer.
    loadSpawnInfo();
    unchecked((g_startupArgs[0] = g_spawnProcEndpoint));
    unchecked((g_startupArgs[1] = 0));
    unchecked((g_startupArgs[2] = 0));
    unchecked((g_startupArgs[3] = 0));
    const rc = entry(readSpawnArgs());
    proc_exit(rc);
    return rc;
}

function writeBytes(bytes: Uint8Array): bool {
    if (bytes.length == 0) {
        return true;
    }
    return console_write(bytes.dataStart as i32, bytes.length) == 0;
}

function writeStringRaw(text: string): bool {
    const buffer = Uint8Array.wrap(String.UTF8.encode(text, false));
    return writeBytes(buffer);
}

function ensureIpcReplyEndpoint(): i32 {
    if (g_ipcReplyEndpoint >= 0) {
        return g_ipcReplyEndpoint;
    }
    g_ipcReplyEndpoint = ipc_create_endpoint();
    return g_ipcReplyEndpoint;
}

function nextIpcRequestId(): i32 {
    const id = g_ipcRequestId;
    g_ipcRequestId += 1;
    if (g_ipcRequestId < 1) {
        g_ipcRequestId = 1;
    }
    return id;
}

function ensureFsReplyEndpoint(): i32 {
    if (g_fsReplyEndpoint >= 0) {
        return g_fsReplyEndpoint;
    }
    g_fsReplyEndpoint = ipc_create_endpoint();
    return g_fsReplyEndpoint;
}

function nextFsRequestId(): i32 {
    const requestId = g_fsRequestId;
    g_fsRequestId += 1;
    if (g_fsRequestId < 1) {
        g_fsRequestId = 1;
    }
    return requestId;
}

class FsResponse {
    constructor(
        public arg0: i32 = 0,
        public arg1: i32 = 0,
    ) {}
}

/**
 * Result of fs.stat: `size` is the file length in bytes, `mode` carries only the
 * S_IFREG / S_IFDIR type bits.
 */
export class FileStat {
    constructor(
        public size: i32 = 0,
        public mode: i32 = 0,
    ) {}
}

function fsRequest(type: i32, arg0: i32, arg1: i32, arg2: i32, arg3: i32): FsResponse | null {
    const endpoint = fs_endpoint();
    const replyEndpoint = ensureFsReplyEndpoint();
    if (endpoint < 0 || replyEndpoint < 0) {
        return null;
    }

    const requestId = nextFsRequestId();
    if (ipc_send(endpoint, replyEndpoint, type, requestId, arg0, arg1, arg2, arg3) != 0) {
        return null;
    }
    if (ipc_recv(replyEndpoint) < 0) {
        return null;
    }
    if (
        ipc_last_field(IPC_FIELD_REQUEST_ID) != requestId ||
        ipc_last_field(IPC_FIELD_TYPE) != FS_IPC_RESP
    ) {
        return null;
    }
    return new FsResponse(ipc_last_field(IPC_FIELD_ARG0), ipc_last_field(IPC_FIELD_ARG1));
}

function fsRequestStream(
    type: i32,
    arg0: i32,
    arg1: i32,
    arg2: i32,
    arg3: i32,
    out: Uint8Array,
): i32 {
    const endpoint = fs_endpoint();
    const replyEndpoint = ensureFsReplyEndpoint();
    if (endpoint < 0 || replyEndpoint < 0 || out.length == 0) {
        return -1;
    }

    const requestId = nextFsRequestId();
    if (ipc_send(endpoint, replyEndpoint, type, requestId, arg0, arg1, arg2, arg3) != 0) {
        return -1;
    }

    let outLen: i32 = 0;
    while (true) {
        if (ipc_recv(replyEndpoint) < 0) {
            return -1;
        }
        if (ipc_last_field(IPC_FIELD_REQUEST_ID) != requestId) {
            continue;
        }

        const respType = ipc_last_field(IPC_FIELD_TYPE);
        if (respType == FS_IPC_STREAM) {
            const a0 = ipc_last_field(IPC_FIELD_ARG0);
            const a1 = ipc_last_field(IPC_FIELD_ARG1);
            const a2 = ipc_last_field(IPC_FIELD_ARG2);
            const a3 = ipc_last_field(IPC_FIELD_ARG3);
            const bytes = [a0, a1, a2, a3];
            for (let i = 0; i < 4; ++i) {
                const c = <u8>(bytes[i] & 0xff);
                if (c == 0) {
                    continue;
                }
                if (outLen + 1 >= out.length) {
                    out[out.length - 1] = 0;
                    return outLen;
                }
                out[outLen] = c;
                outLen += 1;
            }
            continue;
        }

        if (respType != FS_IPC_RESP || ipc_last_field(IPC_FIELD_ARG0) != 0) {
            return -1;
        }
        out[outLen] = 0;
        return outLen;
    }
}

/**
 * Console output and line input. Everything here goes to the process's console
 * (the kernel log / its terminal), not to a file descriptor, and every write
 * returns true on success and false when the console host call refused it.
 */
export namespace std {
    /** Writes `text` as UTF-8 with no trailing newline and no NUL. */
    export function write(text: string): bool {
        return writeStringRaw(text);
    }

    /** Identical to `write`: no newline is appended, unlike C's puts. */
    export function puts(text: string): bool {
        return writeStringRaw(text);
    }

    /**
     * Writes `text` verbatim: this port does no formatting and takes no format
     * arguments. The name is kept for parity with the other language ports;
     * build the string with AssemblyScript's own concatenation.
     */
    export function printf(text: string): bool {
        return writeStringRaw(text);
    }

    /** Writes `text` followed by a newline. */
    export function println(text: string): bool {
        return writeStringRaw(text + "\n");
    }

    /**
     * Reads console bytes up to and including the first newline and returns them
     * decoded as UTF-8, or null when `maxLen` is under 2 or a read failed.
     *
     * The newline, when one arrived, is part of the result. Reads stop as soon
     * as the console has no byte ready, so a short line is normal and does not
     * mean end of input; this does not park until a full line exists. `maxLen`
     * is the scratch capacity, capped at 1024, and the whole scratch buffer is
     * decoded -- so the result carries the NUL terminator and the unused
     * capacity as trailing U+0000 characters.
     */
    export function readline(maxLen: i32 = 128): string | null {
        if (maxLen <= 1) {
            return null;
        }
        const limit = maxLen > 1024 ? 1024 : maxLen;
        const out = new Uint8Array(limit);
        let pos: i32 = 0;
        while (pos + 1 < limit) {
            const got = console_read((out.dataStart as i32) + pos, 1);
            if (got < 0) {
                return null;
            }
            if (got == 0) {
                break;
            }
            if (out[pos] == 10) {
                pos += 1;
                break;
            }
            pos += 1;
        }
        out[pos] = 0;
        return String.UTF8.decode(out.buffer, false);
    }
}


@external("wasmos", "io_in8") declare function io_in8_raw(port: i32, out: i32): i32;


@external("wasmos", "io_in16") declare function io_in16_raw(port: i32, out: i32): i32;

/* The port-read host calls report the value through an out-parameter and the
 * outcome through the return, so a refused read is distinguishable from the
 * all-ones an absent device reads back. One scratch cell and one status check
 * live here rather than in every driver.
 *
 * The wrappers fold both back into an i32, which is safe where the host call
 * was not: a byte and a word cannot reach the negative range, so a negative
 * result here is unambiguously the WASMOS_ERR_IO_* code. Callers must test
 * `< 0` -- masking the result throws the distinction away again. */
const g_ioScratch = new Uint8Array(2);

export namespace io {
    /** Reads a byte: 0..255, or a negative WASMOS_ERR_IO_* code. */
    export function in8(port: i32): i32 {
        const ptr = g_ioScratch.dataStart;
        const rc = io_in8_raw(port, ptr as i32);
        return rc != 0 ? rc : <i32>load<u8>(ptr);
    }

    /** Reads a word: 0..65535, or a negative WASMOS_ERR_IO_* code. */
    export function in16(port: i32): i32 {
        const ptr = g_ioScratch.dataStart;
        const rc = io_in16_raw(port, ptr as i32);
        return rc != 0 ? rc : <i32>load<u16>(ptr);
    }
}

/**
 * An open file, wrapping the FS manager's client-side descriptor. Instances come
 * from the fs.open* helpers; the constructor takes a descriptor the caller
 * already owns.
 */
export class File {
    constructor(private fd: i32) {}

    /**
     * Reads at the file's current offset and returns a freshly allocated array
     * of exactly the bytes read: an empty array at end of file, or null on any
     * failure.
     *
     * One request per call, so a short array is normal. `maxLen` bounds the
     * request; 0 or a value above the transfer-buffer size means "as much as one
     * transfer buffer holds". The whole file is read by fs.readFile, which loops
     * over this.
     */
    read(maxLen: i32 = 0): Uint8Array | null {
        const bufferLimit = xfer_buffer_size();
        if (bufferLimit <= 0) {
            return null;
        }

        let requested = maxLen;
        if (requested <= 0 || requested > bufferLimit) {
            requested = bufferLimit;
        }

        // Own a buffer and grant the FS manager R|W so the backend can fill it.
        const bid = xfer_buffer_acquire(requested);
        if (bid < 0) {
            return null;
        }
        const b1 = xfer_buffer_borrow(fs_endpoint(), bid, XFER_GRANT_RW);
        if (b1 < 0) {
            xfer_buffer_release(bid);
            return null;
        }
        const response = fsRequest(FS_IPC_READ_REQ, this.fd, requested, bid, b1);
        if (response == null) {
            xfer_buffer_release(bid);
            return null;
        }
        const readLen = response.arg0;
        if (readLen < 0 || readLen > requested) {
            xfer_buffer_release(bid);
            return null;
        }
        if (readLen == 0) {
            xfer_buffer_release(bid);
            return new Uint8Array(0);
        }

        const buffer = new Uint8Array(readLen);
        const rc = xfer_buffer_read(bid, buffer.dataStart as i32, readLen, 0);
        xfer_buffer_release(bid);
        if (rc != 0) {
            return null;
        }
        return buffer;
    }

    /**
     * Releases the descriptor at the FS manager. True only when the manager
     * reported success; the descriptor is not cleared here, so closing twice
     * sends a second request.
     */
    close(): bool {
        const response = fsRequest(FS_IPC_CLOSE_REQ, this.fd, 0, 0, 0);
        return response != null && response.arg0 == 0;
    }

    /**
     * Writes `buffer` at the file's current offset and returns how many bytes
     * the FS manager accepted, or -1 when nothing at all could be written.
     *
     * Chunked over the transfer buffer: a chunk only partially accepted ends the
     * loop, so a short return is a real short write. A failure after some bytes
     * went out reports those bytes rather than the error, so a return below
     * `buffer.length` does not distinguish a short write from a failed one.
     */
    write(buffer: Uint8Array): i32 {
        const bufferLimit = xfer_buffer_size();
        if (bufferLimit <= 0) {
            return -1;
        }

        // Own one buffer and grant the FS manager once; reuse both across the whole
        // chunk loop (a per-chunk re-grant would fail ALREADY_BORROWED). release()
        // cascade-revokes the grant.
        const bid = xfer_buffer_acquire(bufferLimit);
        if (bid < 0) {
            return -1;
        }
        const b1 = xfer_buffer_borrow(fs_endpoint(), bid, XFER_GRANT_RW);
        if (b1 < 0) {
            xfer_buffer_release(bid);
            return -1;
        }
        let done = 0;
        let failed = false;
        while (done < buffer.length) {
            let chunkLen = buffer.length - done;
            if (chunkLen > bufferLimit) {
                chunkLen = bufferLimit;
            }
            if (xfer_buffer_write(bid, (buffer.dataStart as i32) + done, chunkLen, 0) != 0) {
                failed = true;
                break;
            }
            const response = fsRequest(FS_IPC_WRITE_REQ, this.fd, chunkLen, bid, b1);
            if (response == null || response.arg0 < 0 || response.arg0 > chunkLen) {
                failed = true;
                break;
            }
            done += response.arg0;
            if (response.arg0 == 0 || response.arg0 != chunkLen) {
                break;
            }
        }
        xfer_buffer_release(bid);
        if (failed && done == 0) {
            return -1;
        }
        return done;
    }

    /**
     * Moves the file offset to `offset` bytes from the SEEK_SET / SEEK_CUR /
     * SEEK_END origin and returns the new absolute offset, or -1. A target
     * outside [0, size] is refused by the backend; seeking past the end does not
     * extend the file.
     */
    seek(offset: i32, whence: i32): i32 {
        const response = fsRequest(FS_IPC_SEEK_REQ, this.fd, offset, whence, 0);
        if (response == null || response.arg0 < 0) {
            return -1;
        }
        return response.arg0;
    }
}

/**
 * Synchronous message passing. `call` and `recv` park the process in the kernel
 * until a message arrives; a component that must keep serving its own endpoint
 * while a request is outstanding uses the EventLoop in eventloop.ts instead.
 */
export namespace ipc {
    /**
     * A received message. The four argument words are protocol-defined; `source`
     * is the endpoint to address a reply to, `destination` the endpoint it
     * arrived on.
     */
    export class Reply {
        constructor(
            public type: i32 = 0,
            public requestId: i32 = 0,
            public source: i32 = 0,
            public destination: i32 = 0,
            public arg0: i32 = 0,
            public arg1: i32 = 0,
            public arg2: i32 = 0,
            public arg3: i32 = 0,
        ) {}
    }

    // Create a new message endpoint (for servers setting up their receive endpoint).
    export function createEndpoint(): i32 {
        return ipc_create_endpoint();
    }

    // Send a request to server and block until the reply carrying this call's
    // request id arrives on the per-context managed reply endpoint. Anything
    // else that lands there meanwhile is consumed and DISCARDED; a component
    // that must not lose those messages uses the EventLoop in eventloop.ts.
    export function call(
        server: i32,
        type: i32,
        arg0: i32,
        arg1: i32,
        arg2: i32,
        arg3: i32,
    ): Reply | null {
        const replyEndpoint = ensureIpcReplyEndpoint();
        if (server < 0 || replyEndpoint < 0) {
            return null;
        }
        const requestId = nextIpcRequestId();
        if (ipc_send(server, replyEndpoint, type, requestId, arg0, arg1, arg2, arg3) != 0) {
            return null;
        }
        while (true) {
            if (ipc_recv(replyEndpoint) < 0) {
                return null;
            }
            if (ipc_last_field(IPC_FIELD_REQUEST_ID) != requestId) {
                continue;
            }
            return new Reply(
                ipc_last_field(IPC_FIELD_TYPE),
                ipc_last_field(IPC_FIELD_REQUEST_ID),
                ipc_last_field(IPC_FIELD_SOURCE),
                ipc_last_field(IPC_FIELD_DESTINATION),
                ipc_last_field(IPC_FIELD_ARG0),
                ipc_last_field(IPC_FIELD_ARG1),
                ipc_last_field(IPC_FIELD_ARG2),
                ipc_last_field(IPC_FIELD_ARG3),
            );
        }
    }

    // Block until a message arrives on endpoint (for servers).
    // Parks the process indefinitely: no timeout, no interruption. Every message
    // queued on the endpoint is returned, replies and requests alike, so a
    // server that also issues requests must demultiplex on requestId itself.
    // Null for a negative endpoint or a receive error.
    export function recv(endpoint: i32): Reply | null {
        if (endpoint < 0) {
            return null;
        }
        if (ipc_recv(endpoint) < 0) {
            return null;
        }
        return new Reply(
            ipc_last_field(IPC_FIELD_TYPE),
            ipc_last_field(IPC_FIELD_REQUEST_ID),
            ipc_last_field(IPC_FIELD_SOURCE),
            ipc_last_field(IPC_FIELD_DESTINATION),
            ipc_last_field(IPC_FIELD_ARG0),
            ipc_last_field(IPC_FIELD_ARG1),
            ipc_last_field(IPC_FIELD_ARG2),
            ipc_last_field(IPC_FIELD_ARG3),
        );
    }

    // Send a reply from a server back to the caller's private reply endpoint.
    // source should be the server's own service endpoint.
    // destination should be req.source from the incoming request.
    // requestId must be echoed from the request or the caller cannot match the
    // reply. True once the message is queued -- not once the peer has read it;
    // false means the send itself was refused.
    export function reply(
        destination: i32,
        source: i32,
        type: i32,
        requestId: i32,
        arg0: i32,
        arg1: i32,
        arg2: i32,
        arg3: i32,
    ): bool {
        return ipc_send(destination, source, type, requestId, arg0, arg1, arg2, arg3) == 0;
    }
}

/**
 * Synchronous filesystem access over the FS manager's IPC protocol. Every call
 * stages its payload through an owned transfer buffer, sends one request and
 * parks until the reply arrives.
 *
 * Failures collapse to null / false: a request the backend refuses comes back as
 * an FS error message rather than a response, so the packed WASMOS_ERR_FS_*
 * status it carries is not surfaced.
 */
export namespace fs {
    // Owner-push staging: own a buffer holding the NUL-terminated path, grant the
    // FS manager R|W over it, and return the handles + path length (excluding NUL).
    // The caller passes pathLen (arg0), bid (arg2) and b1 (arg3) to fsRequest, and
    // releases bid afterward; fs-manager unborrows b1 before replying.
    class StagedPath {
        constructor(
            public bid: i32,
            public b1: i32,
            public pathLen: i32,
        ) {}
    }

    function stagePath(path: string): StagedPath | null {
        const pathBytes = Uint8Array.wrap(String.UTF8.encode(path, true));
        const bufferLimit = xfer_buffer_size();
        if (bufferLimit <= 0 || pathBytes.length > bufferLimit) {
            return null;
        }
        const bid = xfer_buffer_acquire(pathBytes.length);
        if (bid < 0) {
            return null;
        }
        if (xfer_buffer_write(bid, pathBytes.dataStart as i32, pathBytes.length, 0) != 0) {
            xfer_buffer_release(bid);
            return null;
        }
        const b1 = xfer_buffer_borrow(fs_endpoint(), bid, XFER_GRANT_RW);
        if (b1 < 0) {
            xfer_buffer_release(bid);
            return null;
        }
        return new StagedPath(bid, b1, pathBytes.length - 1);
    }

    function openWithFlags(path: string, flags: i32): File | null {
        const s = stagePath(path);
        if (s == null) {
            return null;
        }
        const response = fsRequest(FS_IPC_OPEN_REQ, s.pathLen, flags, s.bid, s.b1);
        xfer_buffer_release(s.bid);
        if (response == null || response.arg0 < 0) {
            return null;
        }
        return new File(response.arg0);
    }

    /**
     * Opens an existing file for reading. Null when the path is missing, does
     * not fit the transfer buffer, or the open was refused.
     */
    export function openRead(path: string): File | null {
        return openWithFlags(path, O_RDONLY);
    }

    /**
     * Opens an existing file for writing at offset 0 without truncating it.
     * Does not create the file.
     */
    export function openWrite(path: string): File | null {
        return openWithFlags(path, O_WRONLY);
    }

    /** Opens for writing, creating the file if needed and truncating it. */
    export function create(path: string): File | null {
        return openWithFlags(path, O_WRONLY | O_CREAT | O_TRUNC);
    }

    /** Opens for writing, creating the file if needed and appending to it. */
    export function openAppend(path: string): File | null {
        return openWithFlags(path, O_WRONLY | O_CREAT | O_APPEND);
    }

    /**
     * Returns the size and file-type bits of `path` without opening it, or null
     * when it does not exist.
     */
    export function stat(path: string): FileStat | null {
        const s = stagePath(path);
        if (s == null) {
            return null;
        }
        const response = fsRequest(FS_IPC_STAT_REQ, s.pathLen, 0, s.bid, s.b1);
        xfer_buffer_release(s.bid);
        if (response == null || response.arg0 < 0) {
            return null;
        }
        return new FileStat(response.arg0, response.arg1 & (S_IFREG | S_IFDIR));
    }

    /** Removes a file. False when the backend refused it or staging failed. */
    export function unlink(path: string): bool {
        const s = stagePath(path);
        if (s == null) {
            return false;
        }
        const response = fsRequest(FS_IPC_UNLINK_REQ, s.pathLen, 0, s.bid, s.b1);
        xfer_buffer_release(s.bid);
        return response != null && response.arg0 == 0;
    }

    /** Creates a directory. False when staging or the backend refused it. */
    export function mkdir(path: string): bool {
        const s = stagePath(path);
        if (s == null) {
            return false;
        }
        const response = fsRequest(FS_IPC_MKDIR_REQ, s.pathLen, 0, s.bid, s.b1);
        xfer_buffer_release(s.bid);
        return response != null && response.arg0 == 0;
    }

    /** Removes a directory. False when staging or the backend refused it. */
    export function rmdir(path: string): bool {
        const s = stagePath(path);
        if (s == null) {
            return false;
        }
        const response = fsRequest(FS_IPC_RMDIR_REQ, s.pathLen, 0, s.bid, s.b1);
        xfer_buffer_release(s.bid);
        return response != null && response.arg0 == 0;
    }

    /**
     * Lists the current directory as text, or null on failure.
     *
     * The listing arrives as a stream of messages carrying four bytes each, and
     * zero bytes inside a message are skipped rather than stored. `maxLen` is
     * the scratch capacity; a listing that does not fit is truncated and the
     * remaining stream messages keep arriving on the reply endpoint undrained.
     */
    export function readDir(maxLen: i32 = 512): string | null {
        if (maxLen <= 1) {
            return null;
        }
        const out = new Uint8Array(maxLen);
        const got = fsRequestStream(FS_IPC_READDIR_REQ, 0, 0, 0, 0, out);
        if (got < 0) {
            return null;
        }
        return String.UTF8.decodeUnsafe(out.dataStart, got, false);
    }

    /**
     * Opens `path`, reads it to the end and returns the whole contents, or null
     * when the open or any read failed (the file is closed either way).
     *
     * Chunks are accumulated and then copied into one array, so the file is held
     * twice in memory at the end of the call. The loop stops at the first chunk
     * shorter than one transfer buffer, which is what marks the end of the file
     * for this protocol.
     */
    export function readFile(path: string): Uint8Array | null {
        const file = openRead(path);
        if (file == null) {
            return null;
        }

        const chunks = new Array<Uint8Array>();
        let total = 0;
        while (true) {
            const chunk = file.read();
            if (chunk == null) {
                file.close();
                return null;
            }
            if (chunk.length == 0) {
                break;
            }
            chunks.push(chunk);
            total += chunk.length;
            if (chunk.length < xfer_buffer_size()) {
                break;
            }
        }
        file.close();

        const output = new Uint8Array(total);
        let offset = 0;
        for (let i = 0; i < chunks.length; ++i) {
            const chunk = chunks[i];
            memory.copy(output.dataStart + offset, chunk.dataStart, chunk.length);
            offset += chunk.length;
        }
        return output;
    }

    /**
     * `readFile` decoded as UTF-8, without treating a NUL as a terminator: a
     * binary file comes back as a string with embedded U+0000 characters.
     */
    export function readTextFile(path: string): string | null {
        const bytes = readFile(path);
        if (bytes == null) {
            return null;
        }
        return String.UTF8.decodeUnsafe(bytes.dataStart, bytes.length, false);
    }
}
