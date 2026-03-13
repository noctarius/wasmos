#![allow(dead_code)]

use core::fmt::{self, Write};

const FS_IPC_OPEN_REQ: i32 = 0x400;
const FS_IPC_READ_REQ: i32 = 0x401;
const FS_IPC_CLOSE_REQ: i32 = 0x402;
const FS_IPC_RESP: i32 = 0x480;

const IPC_FIELD_TYPE: i32 = 0;
const IPC_FIELD_REQUEST_ID: i32 = 1;
const IPC_FIELD_ARG0: i32 = 2;

#[link(wasm_import_module = "wasmos")]
unsafe extern "C" {
    fn console_write(ptr: i32, len: i32) -> i32;
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
    fn ipc_recv(endpoint: i32) -> i32;
    fn ipc_last_field(field: i32) -> i32;
    fn fs_endpoint() -> i32;
    fn fs_buffer_size() -> i32;
    fn fs_buffer_write(ptr: i32, len: i32, offset: i32) -> i32;
    fn fs_buffer_copy(ptr: i32, len: i32, offset: i32) -> i32;
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Error {
    BadResponse,
    BufferTooSmall,
    HostCallFailed,
    InvalidArgument,
    NameTooLong,
    NotAvailable,
}

static mut G_FS_REPLY_ENDPOINT: i32 = -1;
static mut G_FS_REQUEST_ID: i32 = 1;
static mut G_STARTUP_ARGS: [i32; 4] = [0; 4];

pub mod startup {
    use super::G_STARTUP_ARGS;

    pub fn arg(index: usize) -> i32 {
        if index >= 4 {
            return 0;
        }
        unsafe { G_STARTUP_ARGS[index] }
    }
}

static EMPTY_ARGS: [&str; 0] = [];

#[no_mangle]
pub extern "C" fn wasmos_main(arg0: i32, arg1: i32, arg2: i32, arg3: i32) -> i32 {
    unsafe {
        G_STARTUP_ARGS[0] = arg0;
        G_STARTUP_ARGS[1] = arg1;
        G_STARTUP_ARGS[2] = arg2;
        G_STARTUP_ARGS[3] = arg3;
    }
    crate::main(&EMPTY_ARGS)
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

fn fs_request(msg_type: i32, arg0: i32, arg1: i32, arg2: i32, arg3: i32) -> Result<i32, Error> {
    let endpoint = unsafe { fs_endpoint() };
    if endpoint < 0 {
        return Err(Error::NotAvailable);
    }

    let reply_endpoint = ensure_fs_reply_endpoint()?;
    let request_id = next_fs_request_id();

    if unsafe { ipc_send(endpoint, reply_endpoint, msg_type, request_id, arg0, arg1, arg2, arg3) } != 0 {
        return Err(Error::HostCallFailed);
    }
    if unsafe { ipc_recv(reply_endpoint) } < 0 {
        return Err(Error::HostCallFailed);
    }

    let response_request_id = unsafe { ipc_last_field(IPC_FIELD_REQUEST_ID) };
    let response_type = unsafe { ipc_last_field(IPC_FIELD_TYPE) };
    if response_request_id != request_id || response_type != FS_IPC_RESP {
        return Err(Error::BadResponse);
    }

    Ok(unsafe { ipc_last_field(IPC_FIELD_ARG0) })
}

pub mod std {
    use super::{fmt, raw_write, Error, Write};

    pub struct Writer;

    impl Write for Writer {
        fn write_str(&mut self, s: &str) -> fmt::Result {
            raw_write(s.as_bytes()).map_err(|_| fmt::Error)
        }
    }

    pub fn write(bytes: &[u8]) -> Result<(), Error> {
        raw_write(bytes)
    }

    pub fn puts(bytes: &[u8]) -> Result<(), Error> {
        raw_write(bytes)
    }

    pub fn print(args: fmt::Arguments<'_>) -> Result<(), Error> {
        let mut writer = Writer;
        writer.write_fmt(args).map_err(|_| Error::HostCallFailed)
    }

    pub fn printf(args: fmt::Arguments<'_>) -> Result<(), Error> {
        print(args)
    }
}

pub mod fs {
    use super::{fs_buffer_copy, fs_buffer_size, fs_buffer_write, fs_request, Error, FS_IPC_CLOSE_REQ, FS_IPC_OPEN_REQ, FS_IPC_READ_REQ};

    pub struct File {
        fd: i32,
    }

    impl File {
        pub fn read(&self, buffer: &mut [u8]) -> Result<usize, Error> {
            if buffer.is_empty() {
                return Ok(0);
            }

            let max_buffer = unsafe { fs_buffer_size() };
            if max_buffer <= 0 {
                return Err(Error::NotAvailable);
            }

            let mut done = 0usize;
            while done < buffer.len() {
                let remaining = buffer.len() - done;
                let chunk_len = remaining.min(max_buffer as usize);
                let chunk_read = fs_request(FS_IPC_READ_REQ, self.fd, chunk_len as i32, 0, 0)?;
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
                if unsafe { fs_buffer_copy(dst_ptr, chunk_read, 0) } != 0 {
                    return Err(Error::HostCallFailed);
                }
                done += chunk_read as usize;
                if chunk_read as usize != chunk_len {
                    break;
                }
            }

            Ok(done)
        }

        pub fn close(self) -> Result<(), Error> {
            let _ = fs_request(FS_IPC_CLOSE_REQ, self.fd, 0, 0, 0)?;
            Ok(())
        }
    }

    pub fn open_read(path: &str) -> Result<File, Error> {
        let path_bytes = path.as_bytes();
        let max_buffer = unsafe { fs_buffer_size() };
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

        if unsafe { fs_buffer_write(path_buf.as_ptr() as i32, (path_bytes.len() + 1) as i32, 0) } != 0 {
            return Err(Error::HostCallFailed);
        }

        let fd = fs_request(FS_IPC_OPEN_REQ, path_bytes.len() as i32, 0, 0, 0)?;
        if fd < 0 {
            return Err(Error::BadResponse);
        }

        Ok(File { fd })
    }
}
