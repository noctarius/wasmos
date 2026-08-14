#![allow(dead_code)]

use core::fmt::{self, Write};

/// Stackless coroutines, futures/promises, the IPC event loop and the typed
/// asynchronous filesystem operations; see `coroutine.rs` for the contracts.
pub mod coroutine;

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

// `whence` values for fs::File::seek: the new offset is measured from the start
// of the file, from the current offset, or from the file size. The FS backend
// refuses a resulting offset outside [0, size], so seeking past the end fails
// instead of extending the file.
pub const SEEK_SET: i32 = 0;
pub const SEEK_CUR: i32 = 1;
pub const SEEK_END: i32 = 2;
// File-type bits of fs::Stat::mode; `stat` masks off everything else, and the
// FS reply carries no permission bits.
pub const S_IFREG: u32 = 0x8000;
pub const S_IFDIR: u32 = 0x4000;
// Open flags, POSIX-valued. Bit 0 is the access mode (O_RDONLY or O_WRONLY);
// the remaining flags are modifiers the FS backend accepts only alongside
// O_WRONLY. There is no read/write mode: any other bit is rejected.
pub const O_RDONLY: i32 = 0;
pub const O_WRONLY: i32 = 1;
pub const O_APPEND: i32 = 0x0008;
pub const O_CREAT: i32 = 0x0040;
pub const O_TRUNC: i32 = 0x0200;
const XFER_GRANT_RW: i32 = 0x3;

#[link(wasm_import_module = "wasmos")]
unsafe extern "C" {
    fn console_write(ptr: i32, len: i32) -> i32;
    fn console_read(ptr: i32, len: i32) -> i32;
    fn proc_exit(status: i32) -> i32;
    fn ipc_create_endpoint() -> i32;
    fn ipc_send(
        destination_endpoint: i32,
        source_endpoint: i32,
        msg_type: i32,
        request_id: i32,
        arg0: i32,
        arg1: i32,
        arg2: i32,
        arg3: i32,
    ) -> i32;
    fn ipc_select_one(endpoint: i32) -> i32;
    fn ipc_last_field(field: i32) -> i32;
    fn fs_endpoint() -> i32;
    fn xfer_buffer_size() -> i32;
    fn xfer_buffer_acquire(minimum_size: i32) -> i32;
    fn xfer_buffer_borrow(grantee_endpoint: i32, buffer_id: i32, flags: i32) -> i32;
    fn xfer_buffer_release(buffer_id: i32) -> i32;
    fn xfer_buffer_write(buffer_id: i32, ptr: i32, len: i32, offset: i32) -> i32;
    fn xfer_buffer_read(buffer_id: i32, ptr: i32, len: i32, offset: i32) -> i32;
    fn thread_gettid() -> i32;
    fn thread_yield() -> i32;
    fn mutex_try_lock(ptr: i32) -> i32;
    fn mutex_unlock(ptr: i32) -> i32;
}

/// Failure modes of this module. The filesystem protocol's own packed
/// `WASMOS_ERR_FS_*` status is not surfaced: a request the backend refuses comes
/// back as an FS error message, which every entry point reports as
/// `BadResponse`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Error {
    /// A reply arrived but did not match the request: wrong message type (an
    /// error reply included), wrong request id, or a negative status field.
    BadResponse,
    /// The path plus its NUL does not fit the transfer buffer the FS manager
    /// reads it from.
    BufferTooSmall,
    /// A host call refused the operation (console write, transfer-buffer
    /// read/write/borrow, or IPC send/receive), or formatting failed.
    HostCallFailed,
    /// A caller-supplied argument is unusable: an empty path, or a `readline`
    /// buffer with no room for a byte plus the NUL terminator.
    InvalidArgument,
    /// The path plus its NUL exceeds the 256-byte staging buffer.
    NameTooLong,
    /// A prerequisite does not exist yet: no FS service has registered, no
    /// endpoint could be created, or no transfer buffer could be acquired.
    NotAvailable,
}

static mut G_FS_REPLY_ENDPOINT: i32 = -1;
static mut G_FS_REQUEST_ID: i32 = 1;
static mut G_IPC_REPLY_ENDPOINT: i32 = -1;
static mut G_IPC_REQUEST_ID: i32 = 1;
static mut G_STARTUP_ARGS: [i32; 4] = [0; 4];

/// Values the process manager passed at spawn time.
///
/// Unlike the C, Zig and AssemblyScript ports this module exposes only the entry
/// registers, with none of the spawn-info fields (process manager endpoint, tty,
/// module count/index, argv); see the FIXME on `arg`.
pub mod startup {
    use super::G_STARTUP_ARGS;

    /// The four wasmos_main entry-arg registers, as received.
    ///
    /// FIXME(spawn-info): PM retired the entry-arg bindings and always passes
    /// zeros (pm_apply_entry_bindings in process_manager_spawn.c), so every
    /// index reads 0 here. The C, Zig and AssemblyScript ports instead read the
    /// spawn-info buffer (wasmos_spawn_info.h) via the spawn_info_buffer host
    /// call, where index 0 means proc.endpoint, and expose tty/module
    /// count+index and the argv blob alongside it; this port has none of that,
    /// so a Rust guest cannot reach its process manager endpoint or its argv.
    pub fn arg(index: usize) -> i32 {
        if index >= 4 {
            return 0;
        }
        unsafe { G_STARTUP_ARGS[index] }
    }
}

static EMPTY_ARGS: [&str; 0] = [];

/// Recursive mutex whose state lives in guest memory and whose arbitration is
/// done by the kernel, layout-compatible with `wasmos_mutex_t`.
///
/// The kernel link tables export no `wasmos.mutex_try_lock` / `mutex_unlock`
/// (FIXME(user-mutex-import) in `src/libc/include/wasmos/api.h`), so a module
/// that actually calls these fails to instantiate on an unresolved import.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Mutex {
    /// Thread id of the current owner, 0 when unlocked. Written by the kernel.
    pub owner_tid: u32,
    /// Number of unmatched acquisitions held by the owner.
    pub recursion_depth: u32,
}

impl Mutex {
    /// An unlocked mutex, usable in a `static`.
    pub const fn new() -> Self {
        Self {
            owner_tid: 0,
            recursion_depth: 0,
        }
    }

    /// Resets to the unlocked state. Zeroing a mutex another thread holds loses
    /// that ownership, so only init one nobody has locked.
    pub fn init(&mut self) {
        self.owner_tid = 0;
        self.recursion_depth = 0;
    }

    /// Thread id of the calling thread, as the kernel records it in `owner_tid`.
    pub fn current_tid() -> i32 {
        unsafe { thread_gettid() }
    }

    /// One acquisition attempt: 0 when the mutex is now held by this thread
    /// (raising `recursion_depth` if it already was), 1 when another thread owns
    /// it, negative on error. Never blocks.
    pub fn try_lock(&mut self) -> i32 {
        unsafe { mutex_try_lock(self as *mut Self as usize as i32) }
    }

    /// Acquires the mutex, yielding the thread between attempts while another
    /// owner holds it. Returns 0 once held, or the negative code that ended the
    /// retry loop. A yield-spin, not a sleep.
    pub fn lock(&mut self) -> i32 {
        loop {
            let rc = self.try_lock();
            if rc != 1 {
                return rc;
            }
            unsafe {
                let _ = thread_yield();
            }
        }
    }

    /// Drops one acquisition, releasing the mutex when `recursion_depth` reaches
    /// zero. Returns 0 on success, negative when the caller is not the owner.
    pub fn unlock(&mut self) -> i32 {
        unsafe { mutex_unlock(self as *mut Self as usize as i32) }
    }
}

/// WASM export PM calls instead of `_start`. `main` always receives an empty
/// argument slice: the argv blob lives in the spawn-info buffer, which this port
/// does not read (see `startup::arg`).
#[no_mangle]
pub extern "C" fn wasmos_main(arg0: i32, arg1: i32, arg2: i32, arg3: i32) -> i32 {
    unsafe {
        G_STARTUP_ARGS[0] = arg0;
        G_STARTUP_ARGS[1] = arg1;
        G_STARTUP_ARGS[2] = arg2;
        G_STARTUP_ARGS[3] = arg3;
    }
    let rc = crate::main(&EMPTY_ARGS);
    unsafe {
        let _ = proc_exit(rc);
    }
    rc
}

fn raw_write(bytes: &[u8]) -> Result<(), Error> {
    if bytes.is_empty() {
        return Ok(());
    }

    let result = unsafe { console_write(bytes.as_ptr() as i32, bytes.len() as i32) };
    if result != 0 {
        return Err(Error::HostCallFailed);
    }
    Ok(())
}

fn ensure_ipc_reply_endpoint() -> Result<i32, Error> {
    unsafe {
        if G_IPC_REPLY_ENDPOINT >= 0 {
            return Ok(G_IPC_REPLY_ENDPOINT);
        }
        let ep = ipc_create_endpoint();
        if ep < 0 {
            return Err(Error::NotAvailable);
        }
        G_IPC_REPLY_ENDPOINT = ep;
        Ok(ep)
    }
}

fn next_ipc_request_id() -> i32 {
    unsafe {
        let id = G_IPC_REQUEST_ID;
        G_IPC_REQUEST_ID += 1;
        if G_IPC_REQUEST_ID < 1 {
            G_IPC_REQUEST_ID = 1;
        }
        id
    }
}

fn ensure_fs_reply_endpoint() -> Result<i32, Error> {
    unsafe {
        if G_FS_REPLY_ENDPOINT >= 0 {
            return Ok(G_FS_REPLY_ENDPOINT);
        }

        let endpoint = ipc_create_endpoint();
        if endpoint < 0 {
            return Err(Error::NotAvailable);
        }
        G_FS_REPLY_ENDPOINT = endpoint;
        Ok(endpoint)
    }
}

fn next_fs_request_id() -> i32 {
    unsafe {
        let request_id = G_FS_REQUEST_ID;
        G_FS_REQUEST_ID += 1;
        if G_FS_REQUEST_ID < 1 {
            G_FS_REQUEST_ID = 1;
        }
        request_id
    }
}

fn fs_request(
    msg_type: i32,
    arg0: i32,
    arg1: i32,
    arg2: i32,
    arg3: i32,
) -> Result<(i32, i32), Error> {
    let endpoint = unsafe { fs_endpoint() };
    if endpoint < 0 {
        return Err(Error::NotAvailable);
    }

    let reply_endpoint = ensure_fs_reply_endpoint()?;
    let request_id = next_fs_request_id();

    if unsafe {
        ipc_send(
            endpoint,
            reply_endpoint,
            msg_type,
            request_id,
            arg0,
            arg1,
            arg2,
            arg3,
        )
    } != 0
    {
        return Err(Error::HostCallFailed);
    }
    if unsafe { ipc_select_one(reply_endpoint) } < 0 {
        return Err(Error::HostCallFailed);
    }

    let response_request_id = unsafe { ipc_last_field(IPC_FIELD_REQUEST_ID) };
    let response_type = unsafe { ipc_last_field(IPC_FIELD_TYPE) };
    if response_request_id != request_id || response_type != FS_IPC_RESP {
        return Err(Error::BadResponse);
    }

    Ok((unsafe { ipc_last_field(IPC_FIELD_ARG0) }, unsafe {
        ipc_last_field(IPC_FIELD_ARG1)
    }))
}

fn fs_request_stream(
    msg_type: i32,
    arg0: i32,
    arg1: i32,
    arg2: i32,
    arg3: i32,
    out: &mut [u8],
) -> Result<usize, Error> {
    let endpoint = unsafe { fs_endpoint() };
    if endpoint < 0 || out.is_empty() {
        return Err(Error::NotAvailable);
    }
    let reply_endpoint = ensure_fs_reply_endpoint()?;
    let request_id = next_fs_request_id();
    if unsafe {
        ipc_send(
            endpoint,
            reply_endpoint,
            msg_type,
            request_id,
            arg0,
            arg1,
            arg2,
            arg3,
        )
    } != 0
    {
        return Err(Error::HostCallFailed);
    }

    let mut out_len = 0usize;
    loop {
        if unsafe { ipc_select_one(reply_endpoint) } < 0 {
            return Err(Error::HostCallFailed);
        }
        let response_request_id = unsafe { ipc_last_field(IPC_FIELD_REQUEST_ID) };
        if response_request_id != request_id {
            continue;
        }
        let response_type = unsafe { ipc_last_field(IPC_FIELD_TYPE) };
        if response_type == FS_IPC_STREAM {
            let args = [
                unsafe { ipc_last_field(IPC_FIELD_ARG0) },
                unsafe { ipc_last_field(IPC_FIELD_ARG1) },
                unsafe { ipc_last_field(IPC_FIELD_ARG2) },
                unsafe { ipc_last_field(IPC_FIELD_ARG3) },
            ];
            for a in args {
                let c = (a & 0xFF) as u8;
                if c == 0 {
                    continue;
                }
                if out_len + 1 >= out.len() {
                    out[out.len() - 1] = 0;
                    return Ok(out_len);
                }
                out[out_len] = c;
                out_len += 1;
            }
            continue;
        }
        if response_type != FS_IPC_RESP || unsafe { ipc_last_field(IPC_FIELD_ARG0) } != 0 {
            return Err(Error::BadResponse);
        }
        if out_len < out.len() {
            out[out_len] = 0;
        }
        return Ok(out_len);
    }
}

/// Console output and line input. Everything here goes to the process's console
/// (the kernel log / its terminal), not to a file descriptor.
pub mod std {
    use super::{console_read, fmt, raw_write, Error, Write};

    /// `core::fmt::Write` sink over the console, so `write!` can target it
    /// directly. Each `write_str` is one console host call; no buffering, and a
    /// refused write surfaces as `fmt::Error`.
    pub struct Writer;

    impl Write for Writer {
        fn write_str(&mut self, s: &str) -> fmt::Result {
            raw_write(s.as_bytes()).map_err(|_| fmt::Error)
        }
    }

    /// Writes `bytes` verbatim; no newline is added and no NUL is required. An
    /// empty slice is a no-op success.
    pub fn write(bytes: &[u8]) -> Result<(), Error> {
        raw_write(bytes)
    }

    /// Identical to `write`: the trailing newline C's `puts` adds is not added
    /// here.
    pub fn puts(bytes: &[u8]) -> Result<(), Error> {
        raw_write(bytes)
    }

    /// Formats `args` straight to the console with no intermediate buffer, so
    /// there is no line-length limit and partial output survives a mid-format
    /// failure. Any failure is reported as `Error::HostCallFailed`.
    pub fn print(args: fmt::Arguments<'_>) -> Result<(), Error> {
        let mut writer = Writer;
        writer.write_fmt(args).map_err(|_| Error::HostCallFailed)
    }

    /// Alias of `print`, for guests written against the C API's name.
    pub fn printf(args: fmt::Arguments<'_>) -> Result<(), Error> {
        print(args)
    }

    /// Reads console bytes into `buffer` up to and including the first newline,
    /// NUL-terminates them, and returns the byte count excluding the NUL.
    ///
    /// The newline, when one arrived, is part of the count. Reads stop as soon
    /// as the console has no byte ready, so a short return is normal and does
    /// not mean end of input; this does not park until a full line exists. A
    /// `buffer` shorter than two bytes is `InvalidArgument`, a full buffer ends
    /// the read with the terminator in the last byte, and a host-call failure
    /// clears `buffer[0]` before returning `HostCallFailed`.
    pub fn readline(buffer: &mut [u8]) -> Result<usize, Error> {
        if buffer.len() <= 1 {
            return Err(Error::InvalidArgument);
        }
        let mut pos = 0usize;
        while pos + 1 < buffer.len() {
            let got = unsafe { console_read(buffer[pos..].as_mut_ptr() as i32, 1) };
            if got < 0 {
                buffer[0] = 0;
                return Err(Error::HostCallFailed);
            }
            if got == 0 {
                break;
            }
            pos += 1;
            if buffer[pos - 1] == b'\n' {
                break;
            }
        }
        buffer[pos] = 0;
        Ok(pos)
    }
}

/// Synchronous message passing. `call` and `recv` park the process in the kernel
/// until a message arrives; a component that must keep serving its own endpoint
/// while a request is outstanding uses `coroutine::EventLoop` instead.
pub mod ipc {
    use super::{
        ensure_ipc_reply_endpoint, ipc_create_endpoint, ipc_last_field, ipc_select_one, ipc_send,
        next_ipc_request_id, Error, IPC_FIELD_ARG0, IPC_FIELD_ARG1, IPC_FIELD_ARG2, IPC_FIELD_ARG3,
        IPC_FIELD_DESTINATION, IPC_FIELD_REQUEST_ID, IPC_FIELD_SOURCE, IPC_FIELD_TYPE,
    };

    /// A received message, copied out of the caller's last-received slot. The
    /// four argument words are protocol-defined; `source` is the endpoint to
    /// address a reply to, `destination` the endpoint it arrived on.
    #[derive(Clone, Copy, Debug)]
    pub struct Reply {
        pub r#type: i32,
        pub request_id: i32,
        pub source: i32,
        pub destination: i32,
        pub arg0: i32,
        pub arg1: i32,
        pub arg2: i32,
        pub arg3: i32,
    }

    fn read_reply() -> Reply {
        unsafe {
            Reply {
                r#type: ipc_last_field(IPC_FIELD_TYPE),
                request_id: ipc_last_field(IPC_FIELD_REQUEST_ID),
                source: ipc_last_field(IPC_FIELD_SOURCE),
                destination: ipc_last_field(IPC_FIELD_DESTINATION),
                arg0: ipc_last_field(IPC_FIELD_ARG0),
                arg1: ipc_last_field(IPC_FIELD_ARG1),
                arg2: ipc_last_field(IPC_FIELD_ARG2),
                arg3: ipc_last_field(IPC_FIELD_ARG3),
            }
        }
    }

    /// Send a request to server and block until the FIRST message arrives on the
    /// per-context managed reply endpoint; it is returned as the reply without
    /// checking its request id or source. Only one request may be outstanding on
    /// that endpoint at a time, or a stale reply is returned for a later call.
    /// The C helper (wasmos_ipc_call) matches instead.
    pub fn call(
        server: i32,
        msg_type: i32,
        arg0: i32,
        arg1: i32,
        arg2: i32,
        arg3: i32,
    ) -> Result<Reply, Error> {
        let reply_ep = ensure_ipc_reply_endpoint()?;
        let request_id = next_ipc_request_id();
        if unsafe {
            ipc_send(
                server, reply_ep, msg_type, request_id, arg0, arg1, arg2, arg3,
            )
        } != 0
        {
            return Err(Error::HostCallFailed);
        }
        if unsafe { ipc_select_one(reply_ep) } < 0 {
            return Err(Error::HostCallFailed);
        }
        Ok(read_reply())
    }

    /// Block until a message arrives on endpoint (for servers).
    /// Parks the process indefinitely: no timeout, no interruption. Every
    /// message queued on `endpoint` is returned, replies and requests alike, so
    /// a server that also issues requests must demultiplex on `request_id`
    /// itself. `HostCallFailed` on an invalid endpoint or a receive error.
    pub fn recv(endpoint: i32) -> Result<Reply, Error> {
        if unsafe { ipc_select_one(endpoint) } < 0 {
            return Err(Error::HostCallFailed);
        }
        Ok(read_reply())
    }

    /// Send a reply from a server back to the caller's private reply endpoint.
    /// source should be the server's own service endpoint.
    /// destination should be req.source from the incoming request.
    /// `request_id` must be echoed from the request or the caller cannot match
    /// the reply. Returns once the message is queued, not once the peer has read
    /// it; `HostCallFailed` means the send itself was refused.
    pub fn reply(
        destination: i32,
        source: i32,
        msg_type: i32,
        request_id: i32,
        arg0: i32,
        arg1: i32,
        arg2: i32,
        arg3: i32,
    ) -> Result<(), Error> {
        if unsafe {
            ipc_send(
                destination,
                source,
                msg_type,
                request_id,
                arg0,
                arg1,
                arg2,
                arg3,
            )
        } != 0
        {
            return Err(Error::HostCallFailed);
        }
        Ok(())
    }

    /// Allocate a new message endpoint (for servers).
    pub fn create_endpoint() -> Result<i32, Error> {
        let ep = unsafe { ipc_create_endpoint() };
        if ep < 0 {
            return Err(Error::NotAvailable);
        }
        Ok(ep)
    }
}

/// Synchronous filesystem access over the FS manager's IPC protocol. Every call
/// stages its payload through an owned transfer buffer, sends one request, and
/// parks until the reply arrives.
pub mod fs {
    use super::{
        fs_endpoint, fs_request, fs_request_stream, xfer_buffer_acquire, xfer_buffer_borrow,
        xfer_buffer_read, xfer_buffer_release, xfer_buffer_size, xfer_buffer_write, Error,
        FS_IPC_CLOSE_REQ, FS_IPC_MKDIR_REQ, FS_IPC_OPEN_REQ, FS_IPC_READDIR_REQ, FS_IPC_READ_REQ,
        FS_IPC_RMDIR_REQ, FS_IPC_SEEK_REQ, FS_IPC_STAT_REQ, FS_IPC_UNLINK_REQ, FS_IPC_WRITE_REQ,
        O_APPEND, O_CREAT, O_RDONLY, O_TRUNC, O_WRONLY, S_IFDIR, S_IFREG, XFER_GRANT_RW,
    };
    use crate::wasmos::coroutine::{EventLoop, FsRequest, Future};

    /// Result of `stat`: `size` is the file length in bytes, `mode` carries only
    /// the `S_IFREG` / `S_IFDIR` type bits.
    #[derive(Clone, Copy, Debug, Eq, PartialEq)]
    pub struct Stat {
        pub size: u32,
        pub mode: u32,
    }

    /// An open file, holding the FS manager's client-side descriptor. Dropping
    /// it does not close the file -- only `close` does, and it consumes the
    /// handle so a descriptor cannot be closed twice from here.
    pub struct File {
        fd: i32,
    }

    /// Submit a filesystem protocol request through the non-blocking event
    /// loop. Existing synchronous File APIs remain unchanged. The caller keeps
    /// request and any transfer buffer named by args[2]/args[3] alive until
    /// the returned future settles.
    pub fn request_async<'a>(
        loop_: &'a mut EventLoop,
        request: &'a mut FsRequest,
        reply_endpoint: i32,
        msg_type: i32,
        args: [i32; 4],
    ) -> Result<(&'a mut Future, i32), Error> {
        let endpoint = unsafe { fs_endpoint() };
        if endpoint < 0 || reply_endpoint < 0 {
            return Err(Error::NotAvailable);
        }
        request.init();
        request
            .send(loop_, endpoint, reply_endpoint, msg_type, args)
            .ok_or(Error::HostCallFailed)
    }

    struct BorrowedBuffer {
        bid: i32,
        b1: i32,
    }

    impl Drop for BorrowedBuffer {
        fn drop(&mut self) {
            unsafe {
                let _ = xfer_buffer_release(self.bid);
            }
        }
    }

    struct StagedPath {
        xfer: BorrowedBuffer,
        path_len: usize,
    }

    fn borrow_fs_buffer(size: i32) -> Result<BorrowedBuffer, Error> {
        let bid = unsafe { xfer_buffer_acquire(size) };
        if bid < 0 {
            return Err(Error::NotAvailable);
        }
        let b1 = unsafe { xfer_buffer_borrow(fs_endpoint(), bid, XFER_GRANT_RW) };
        if b1 < 0 {
            unsafe {
                let _ = xfer_buffer_release(bid);
            }
            return Err(Error::HostCallFailed);
        }
        Ok(BorrowedBuffer { bid, b1 })
    }

    impl File {
        /// Reads up to `buffer.len()` bytes at the file's current offset and
        /// returns how many were stored.
        ///
        /// Loops over transfer-buffer-sized chunks, so a short reply ends the
        /// read: 0 means end of file, and a value below `buffer.len()` is not an
        /// error. An empty `buffer` is a 0-byte success that issues no request.
        /// The transfer buffer is released when the call returns, which also
        /// revokes the FS manager's borrow.
        pub fn read(&self, buffer: &mut [u8]) -> Result<usize, Error> {
            if buffer.is_empty() {
                return Ok(0);
            }

            let max_buffer = unsafe { xfer_buffer_size() };
            if max_buffer <= 0 {
                return Err(Error::NotAvailable);
            }
            let xfer = borrow_fs_buffer(max_buffer)?;

            let mut done = 0usize;
            while done < buffer.len() {
                let remaining = buffer.len() - done;
                let chunk_len = remaining.min(max_buffer as usize);
                let (chunk_read, _) = fs_request(
                    FS_IPC_READ_REQ,
                    self.fd,
                    chunk_len as i32,
                    xfer.bid,
                    xfer.b1,
                )?;
                if chunk_read < 0 {
                    return Err(Error::BadResponse);
                }
                if chunk_read == 0 {
                    break;
                }
                if chunk_read > max_buffer || chunk_read as usize > chunk_len {
                    return Err(Error::BadResponse);
                }
                let dst_ptr = unsafe { buffer.as_mut_ptr().add(done) } as i32;
                if unsafe { xfer_buffer_read(xfer.bid, dst_ptr, chunk_read, 0) } != 0 {
                    return Err(Error::HostCallFailed);
                }
                done += chunk_read as usize;
                if chunk_read as usize != chunk_len {
                    break;
                }
            }

            Ok(done)
        }

        /// Releases the descriptor at the FS manager, consuming the handle. A
        /// refusal surfaces as `BadResponse`; the status word of a successful
        /// response is ignored.
        pub fn close(self) -> Result<(), Error> {
            let _ = fs_request(FS_IPC_CLOSE_REQ, self.fd, 0, 0, 0)?;
            Ok(())
        }

        /// Writes `buffer` at the file's current offset and returns how many
        /// bytes the FS manager accepted.
        ///
        /// Chunked like `read`: a chunk only partially accepted ends the loop,
        /// so a short return is a real short write rather than an error. An
        /// empty `buffer` is a 0-byte success that issues no request.
        pub fn write(&self, buffer: &[u8]) -> Result<usize, Error> {
            if buffer.is_empty() {
                return Ok(0);
            }

            let max_buffer = unsafe { xfer_buffer_size() };
            if max_buffer <= 0 {
                return Err(Error::NotAvailable);
            }
            let xfer = borrow_fs_buffer(max_buffer)?;

            let mut done = 0usize;
            while done < buffer.len() {
                let remaining = buffer.len() - done;
                let chunk_len = remaining.min(max_buffer as usize);
                if unsafe {
                    xfer_buffer_write(
                        xfer.bid,
                        buffer.as_ptr().add(done) as i32,
                        chunk_len as i32,
                        0,
                    )
                } != 0
                {
                    return Err(Error::HostCallFailed);
                }
                let (chunk_written, _) = fs_request(
                    FS_IPC_WRITE_REQ,
                    self.fd,
                    chunk_len as i32,
                    xfer.bid,
                    xfer.b1,
                )?;
                if chunk_written < 0 {
                    return Err(Error::BadResponse);
                }
                if chunk_written as usize > chunk_len {
                    return Err(Error::BadResponse);
                }
                done += chunk_written as usize;
                if chunk_written == 0 || chunk_written as usize != chunk_len {
                    break;
                }
            }

            Ok(done)
        }

        /// Moves the file offset to `offset` bytes from the `SEEK_SET` /
        /// `SEEK_CUR` / `SEEK_END` origin and returns the new absolute offset.
        /// A target outside [0, size] is refused by the backend and surfaces as
        /// `BadResponse`; seeking past the end does not extend the file.
        pub fn seek(&self, offset: i32, whence: i32) -> Result<i32, Error> {
            let (position, _) = fs_request(FS_IPC_SEEK_REQ, self.fd, offset, whence, 0)?;
            if position < 0 {
                return Err(Error::BadResponse);
            }
            Ok(position)
        }
    }

    fn stage_path(path: &str) -> Result<StagedPath, Error> {
        let path_bytes = path.as_bytes();
        let max_buffer = unsafe { xfer_buffer_size() };
        let mut path_buf = [0u8; 256];

        if path_bytes.is_empty() {
            return Err(Error::InvalidArgument);
        }
        if max_buffer <= 0 {
            return Err(Error::NotAvailable);
        }
        if path_bytes.len() + 1 > path_buf.len() {
            return Err(Error::NameTooLong);
        }
        if path_bytes.len() + 1 > max_buffer as usize {
            return Err(Error::BufferTooSmall);
        }

        path_buf[..path_bytes.len()].copy_from_slice(path_bytes);
        path_buf[path_bytes.len()] = 0;

        let xfer = borrow_fs_buffer((path_bytes.len() + 1) as i32)?;
        if unsafe {
            xfer_buffer_write(
                xfer.bid,
                path_buf.as_ptr() as i32,
                (path_bytes.len() + 1) as i32,
                0,
            )
        } != 0
        {
            return Err(Error::HostCallFailed);
        }
        Ok(StagedPath {
            xfer,
            path_len: path_bytes.len(),
        })
    }

    fn open_with_flags(path: &str, flags: i32) -> Result<File, Error> {
        let staged = stage_path(path)?;

        let (fd, _) = fs_request(
            FS_IPC_OPEN_REQ,
            staged.path_len as i32,
            flags,
            staged.xfer.bid,
            staged.xfer.b1,
        )?;
        if fd < 0 {
            return Err(Error::BadResponse);
        }

        Ok(File { fd })
    }

    /// Opens an existing file for reading. `path` is borrowed for the call and
    /// must be non-empty and shorter than 256 bytes including its NUL. A missing
    /// file is reported as `BadResponse`.
    pub fn open_read(path: &str) -> Result<File, Error> {
        open_with_flags(path, O_RDONLY)
    }

    /// Opens an existing file for writing at offset 0 without truncating it.
    /// Does not create the file.
    pub fn open_write(path: &str) -> Result<File, Error> {
        open_with_flags(path, O_WRONLY)
    }

    /// Creates the file if needed and truncates it to zero length.
    pub fn create(path: &str) -> Result<File, Error> {
        open_with_flags(path, O_WRONLY | O_CREAT | O_TRUNC)
    }

    /// Creates the file if needed and positions writes at the end.
    pub fn open_append(path: &str) -> Result<File, Error> {
        open_with_flags(path, O_WRONLY | O_CREAT | O_APPEND)
    }

    /// Returns the size and file-type bits of `path` without opening it.
    /// `BadResponse` when the path does not exist.
    pub fn stat(path: &str) -> Result<Stat, Error> {
        let staged = stage_path(path)?;
        let (size, mode) = fs_request(
            FS_IPC_STAT_REQ,
            staged.path_len as i32,
            0,
            staged.xfer.bid,
            staged.xfer.b1,
        )?;
        if size < 0 {
            return Err(Error::BadResponse);
        }

        Ok(Stat {
            size: size as u32,
            mode: mode as u32 & (S_IFREG | S_IFDIR),
        })
    }

    /// Removes a file. A refusal by the backend (missing file, directory,
    /// read-only mount) arrives as an FS error message and surfaces as
    /// `BadResponse`; the status word of a successful response is not inspected.
    pub fn unlink(path: &str) -> Result<(), Error> {
        let staged = stage_path(path)?;
        let _ = fs_request(
            FS_IPC_UNLINK_REQ,
            staged.path_len as i32,
            0,
            staged.xfer.bid,
            staged.xfer.b1,
        )?;
        Ok(())
    }

    /// Creates a directory. Reports failure the same way as `unlink`.
    pub fn mkdir(path: &str) -> Result<(), Error> {
        let staged = stage_path(path)?;
        let _ = fs_request(
            FS_IPC_MKDIR_REQ,
            staged.path_len as i32,
            0,
            staged.xfer.bid,
            staged.xfer.b1,
        )?;
        Ok(())
    }

    /// Removes a directory. Reports failure the same way as `unlink`.
    pub fn rmdir(path: &str) -> Result<(), Error> {
        let staged = stage_path(path)?;
        let _ = fs_request(
            FS_IPC_RMDIR_REQ,
            staged.path_len as i32,
            0,
            staged.xfer.bid,
            staged.xfer.b1,
        )?;
        Ok(())
    }

    /// Lists the current directory into `buffer` as a NUL-terminated text blob
    /// and returns its length excluding the terminator.
    ///
    /// The listing arrives as a stream of messages carrying four bytes each, and
    /// zero bytes inside a message are skipped rather than stored. A `buffer`
    /// too small is filled, terminated and returned truncated -- the remaining
    /// stream messages keep arriving on the reply endpoint and are not drained.
    pub fn read_dir(buffer: &mut [u8]) -> Result<usize, Error> {
        fs_request_stream(FS_IPC_READDIR_REQ, 0, 0, 0, 0, buffer)
    }
}
