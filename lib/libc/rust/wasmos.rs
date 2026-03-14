#![allow(dead_code)]

use core::fmt::{self, Write};

const FS_IPC_OPEN_REQ: i32 = 0x400;
const FS_IPC_READ_REQ: i32 = 0x401;
const FS_IPC_WRITE_REQ: i32 = 0x406;
const FS_IPC_CLOSE_REQ: i32 = 0x402;
const FS_IPC_STAT_REQ: i32 = 0x403;
const FS_IPC_SEEK_REQ: i32 = 0x405;
const FS_IPC_RESP: i32 = 0x480;

const IPC_FIELD_TYPE: i32 = 0;
const IPC_FIELD_REQUEST_ID: i32 = 1;
const IPC_FIELD_ARG0: i32 = 2;
const IPC_FIELD_ARG1: i32 = 3;

pub const SEEK_SET: i32 = 0;
pub const SEEK_CUR: i32 = 1;
pub const SEEK_END: i32 = 2;
pub const S_IFREG: u32 = 0x8000;
pub const S_IFDIR: u32 = 0x4000;
pub const O_RDONLY: i32 = 0;
pub const O_WRONLY: i32 = 1;
pub const O_APPEND: i32 = 0x0008;
pub const O_CREAT: i32 = 0x0040;
pub const O_TRUNC: i32 = 0x0200;

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

fn fs_request(msg_type: i32, arg0: i32, arg1: i32, arg2: i32, arg3: i32) -> Result<(i32, i32), Error> {
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

    Ok((
        unsafe { ipc_last_field(IPC_FIELD_ARG0) },
        unsafe { ipc_last_field(IPC_FIELD_ARG1) },
    ))
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
    use super::{
        fs_buffer_copy, fs_buffer_size, fs_buffer_write, fs_request, Error, FS_IPC_CLOSE_REQ,
        FS_IPC_OPEN_REQ, FS_IPC_READ_REQ, FS_IPC_SEEK_REQ, FS_IPC_STAT_REQ, FS_IPC_WRITE_REQ,
        O_APPEND, O_CREAT, O_RDONLY, O_TRUNC, O_WRONLY, S_IFDIR, S_IFREG,
    };

    #[derive(Clone, Copy, Debug, Eq, PartialEq)]
    pub struct Stat {
        pub size: u32,
        pub mode: u32,
    }

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
                let (chunk_read, _) = fs_request(FS_IPC_READ_REQ, self.fd, chunk_len as i32, 0, 0)?;
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

        pub fn write(&self, buffer: &[u8]) -> Result<usize, Error> {
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
                if unsafe { fs_buffer_write(buffer.as_ptr().add(done) as i32, chunk_len as i32, 0) } != 0 {
                    return Err(Error::HostCallFailed);
                }
                let (chunk_written, _) = fs_request(FS_IPC_WRITE_REQ, self.fd, chunk_len as i32, 0, 0)?;
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

        pub fn seek(&self, offset: i32, whence: i32) -> Result<i32, Error> {
            let (position, _) = fs_request(FS_IPC_SEEK_REQ, self.fd, offset, whence, 0)?;
            if position < 0 {
                return Err(Error::BadResponse);
            }
            Ok(position)
        }
    }

    fn open_with_flags(path: &str, flags: i32) -> Result<File, Error> {
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

        let (fd, _) = fs_request(FS_IPC_OPEN_REQ, path_bytes.len() as i32, flags, 0, 0)?;
        if fd < 0 {
            return Err(Error::BadResponse);
        }

        Ok(File { fd })
    }

    pub fn open_read(path: &str) -> Result<File, Error> {
        open_with_flags(path, O_RDONLY)
    }

    pub fn open_write(path: &str) -> Result<File, Error> {
        open_with_flags(path, O_WRONLY)
    }

    pub fn create(path: &str) -> Result<File, Error> {
        open_with_flags(path, O_WRONLY | O_CREAT | O_TRUNC)
    }

    pub fn open_append(path: &str) -> Result<File, Error> {
        open_with_flags(path, O_WRONLY | O_CREAT | O_APPEND)
    }

    pub fn stat(path: &str) -> Result<Stat, Error> {
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

        let (size, mode) = fs_request(FS_IPC_STAT_REQ, path_bytes.len() as i32, 0, 0, 0)?;
        if size < 0 {
            return Err(Error::BadResponse);
        }

        Ok(Stat {
            size: size as u32,
            mode: mode as u32 & (S_IFREG | S_IFDIR),
        })
    }
}
