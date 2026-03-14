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

@external("wasmos", "console_write")
declare function console_write(ptr: i32, len: i32): i32;
@external("wasmos", "ipc_create_endpoint")
declare function ipc_create_endpoint(): i32;
@external("wasmos", "ipc_send")
declare function ipc_send(
  destination_endpoint: i32,
  source_endpoint: i32,
  type: i32,
  request_id: i32,
  arg0: i32,
  arg1: i32,
  arg2: i32,
  arg3: i32
): i32;
@external("wasmos", "ipc_recv")
declare function ipc_recv(endpoint: i32): i32;
@external("wasmos", "ipc_last_field")
declare function ipc_last_field(field: i32): i32;
@external("wasmos", "fs_endpoint")
declare function fs_endpoint(): i32;
@external("wasmos", "fs_buffer_size")
declare function fs_buffer_size(): i32;
@external("wasmos", "fs_buffer_write")
declare function fs_buffer_write(ptr: i32, len: i32, offset: i32): i32;
@external("wasmos", "fs_buffer_copy")
declare function fs_buffer_copy(ptr: i32, len: i32, offset: i32): i32;

let g_fsReplyEndpoint: i32 = -1;
let g_fsRequestId: i32 = 1;
let g_startupArgs = new StaticArray<i32>(4);

export namespace startup {
  export function arg(index: i32): i32 {
    if (index < 0 || index >= 4) {
      return 0;
    }
    return unchecked(g_startupArgs[index]);
  }
}

export function runMain(
  entry: (args: Array<string>) => i32,
  arg0: i32,
  arg1: i32,
  arg2: i32,
  arg3: i32
): i32 {
  unchecked(g_startupArgs[0] = arg0);
  unchecked(g_startupArgs[1] = arg1);
  unchecked(g_startupArgs[2] = arg2);
  unchecked(g_startupArgs[3] = arg3);
  return entry(new Array<string>());
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
  constructor(public arg0: i32 = 0, public arg1: i32 = 0) {}
}

export class FileStat {
  constructor(public size: i32 = 0, public mode: i32 = 0) {}
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
  if (ipc_last_field(IPC_FIELD_REQUEST_ID) != requestId || ipc_last_field(IPC_FIELD_TYPE) != FS_IPC_RESP) {
    return null;
  }
  return new FsResponse(ipc_last_field(IPC_FIELD_ARG0), ipc_last_field(IPC_FIELD_ARG1));
}

export namespace std {
  export function write(text: string): bool {
    return writeStringRaw(text);
  }

  export function puts(text: string): bool {
    return writeStringRaw(text);
  }

  export function printf(text: string): bool {
    return writeStringRaw(text);
  }

  export function println(text: string): bool {
    return writeStringRaw(text + "\n");
  }
}

export class File {
  constructor(private fd: i32) {}

  read(maxLen: i32 = 0): Uint8Array | null {
    const bufferLimit = fs_buffer_size();
    if (bufferLimit <= 0) {
      return null;
    }

    let requested = maxLen;
    if (requested <= 0 || requested > bufferLimit) {
      requested = bufferLimit;
    }

    const response = fsRequest(FS_IPC_READ_REQ, this.fd, requested, 0, 0);
    if (response == null) {
      return null;
    }
    const readLen = response.arg0;
    if (readLen < 0 || readLen > requested) {
      return null;
    }
    if (readLen == 0) {
      return new Uint8Array(0);
    }

    const buffer = new Uint8Array(readLen);
    if (fs_buffer_copy(buffer.dataStart as i32, readLen, 0) != 0) {
      return null;
    }
    return buffer;
  }

  close(): bool {
    const response = fsRequest(FS_IPC_CLOSE_REQ, this.fd, 0, 0, 0);
    return response != null && response.arg0 == 0;
  }

  write(buffer: Uint8Array): i32 {
    const bufferLimit = fs_buffer_size();
    if (bufferLimit <= 0) {
      return -1;
    }

    let done = 0;
    while (done < buffer.length) {
      let chunkLen = buffer.length - done;
      if (chunkLen > bufferLimit) {
        chunkLen = bufferLimit;
      }
      if (fs_buffer_write(buffer.dataStart as i32 + done, chunkLen, 0) != 0) {
        return -1;
      }
      const response = fsRequest(FS_IPC_WRITE_REQ, this.fd, chunkLen, 0, 0);
      if (response == null || response.arg0 < 0 || response.arg0 > chunkLen) {
        return -1;
      }
      done += response.arg0;
      if (response.arg0 == 0 || response.arg0 != chunkLen) {
        break;
      }
    }
    return done;
  }

  seek(offset: i32, whence: i32): i32 {
    const response = fsRequest(FS_IPC_SEEK_REQ, this.fd, offset, whence, 0);
    if (response == null || response.arg0 < 0) {
      return -1;
    }
    return response.arg0;
  }
}

export namespace fs {
  function openWithFlags(path: string, flags: i32): File | null {
    const pathBytes = Uint8Array.wrap(String.UTF8.encode(path, true));
    const bufferLimit = fs_buffer_size();
    if (bufferLimit <= 0 || pathBytes.length > bufferLimit) {
      return null;
    }
    if (fs_buffer_write(pathBytes.dataStart as i32, pathBytes.length, 0) != 0) {
      return null;
    }

    const response = fsRequest(FS_IPC_OPEN_REQ, pathBytes.length - 1, flags, 0, 0);
    if (response == null || response.arg0 < 0) {
      return null;
    }
    return new File(response.arg0);
  }

  export function openRead(path: string): File | null {
    return openWithFlags(path, O_RDONLY);
  }

  export function openWrite(path: string): File | null {
    return openWithFlags(path, O_WRONLY);
  }

  export function create(path: string): File | null {
    return openWithFlags(path, O_WRONLY | O_CREAT | O_TRUNC);
  }

  export function openAppend(path: string): File | null {
    return openWithFlags(path, O_WRONLY | O_CREAT | O_APPEND);
  }

  export function stat(path: string): FileStat | null {
    const pathBytes = Uint8Array.wrap(String.UTF8.encode(path, true));
    const bufferLimit = fs_buffer_size();
    if (bufferLimit <= 0 || pathBytes.length > bufferLimit) {
      return null;
    }
    if (fs_buffer_write(pathBytes.dataStart as i32, pathBytes.length, 0) != 0) {
      return null;
    }

    const response = fsRequest(FS_IPC_STAT_REQ, pathBytes.length - 1, 0, 0, 0);
    if (response == null || response.arg0 < 0) {
      return null;
    }
    return new FileStat(response.arg0, response.arg1 & (S_IFREG | S_IFDIR));
  }

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
      if (chunk.length < fs_buffer_size()) {
        break;
      }
    }
    file.close();

    const output = new Uint8Array(total);
    let offset = 0;
    for (let i = 0; i < chunks.length; ++i) {
      const chunk = chunks[i];
      memory.copy(
        output.dataStart + offset,
        chunk.dataStart,
        chunk.length
      );
      offset += chunk.length;
    }
    return output;
  }

  export function readTextFile(path: string): string | null {
    const bytes = readFile(path);
    if (bytes == null) {
      return null;
    }
    return String.UTF8.decodeUnsafe(bytes.dataStart, bytes.length, false);
  }
}
