package main

import "unsafe"

const (
	fsIPCOpenReq   int32 = 0x400
	fsIPCReadReq   int32 = 0x401
	fsIPCWriteReq  int32 = 0x406
	fsIPCCloseReq  int32 = 0x402
	fsIPCStatReq   int32 = 0x403
	fsIPCSeekReq   int32 = 0x405
	fsIPCUnlinkReq int32 = 0x407
	fsIPCMkdirReq  int32 = 0x408
	fsIPCRmdirReq  int32 = 0x409
	fsIPCResp      int32 = 0x480
)

const (
	ipcFieldType      int32 = 0
	ipcFieldRequestID int32 = 1
	ipcFieldArg0      int32 = 2
	ipcFieldArg1      int32 = 3
)

const (
	SeekSet  int32 = 0
	SeekCur  int32 = 1
	SeekEnd  int32 = 2
	SIFREG   int32 = 0x8000
	SIFDIR   int32 = 0x4000
	O_RDONLY int32 = 0
	O_WRONLY int32 = 1
	O_APPEND int32 = 0x0008
	O_CREAT  int32 = 0x0040
	O_TRUNC  int32 = 0x0200
)

type Error int32

const (
	ErrOK Error = iota
	ErrBadResponse
	ErrBufferTooSmall
	ErrHostCallFailed
	ErrInvalidArgument
	ErrNameTooLong
	ErrNotAvailable
)

//go:wasmimport wasmos console_write
func consoleWrite(ptr uint32, len uint32) int32
//go:wasmimport wasmos console_read
func consoleRead(ptr uint32, len uint32) int32

//go:wasmimport wasmos ipc_create_endpoint
func ipcCreateEndpoint() int32

//go:wasmimport wasmos ipc_send
func ipcSend(destinationEndpoint int32, sourceEndpoint int32, msgType int32, requestID int32, arg0 int32, arg1 int32, arg2 int32, arg3 int32) int32

//go:wasmimport wasmos ipc_recv
func ipcRecv(endpoint int32) int32

//go:wasmimport wasmos ipc_last_field
func ipcLastField(field int32) int32

//go:wasmimport wasmos fs_endpoint
func fsEndpoint() int32

//go:wasmimport wasmos fs_buffer_size
func fsBufferSize() int32

//go:wasmimport wasmos fs_buffer_write
func fsBufferWrite(ptr uint32, len uint32, offset uint32) int32

//go:wasmimport wasmos fs_buffer_copy
func fsBufferCopy(ptr uint32, len uint32, offset uint32) int32

var fsReplyEndpoint int32 = -1
var fsRequestID int32 = 1
var startupArgs [4]int32
var startup = startupAPI{}

type stdAPI struct{}

var std = stdAPI{}

type File struct {
	fd int32
}

type FileStat struct {
	Size int32
	Mode int32
}

type fsAPI struct{}

var fs = fsAPI{}

func stagePath(path string) (int, Error) {
	pathLen := len(path)
	maxBuffer := fsBufferSize()
	var pathBuf [256]byte

	if pathLen == 0 {
		return 0, ErrInvalidArgument
	}
	if maxBuffer <= 0 {
		return 0, ErrNotAvailable
	}
	if pathLen+1 > len(pathBuf) {
		return 0, ErrNameTooLong
	}
	if pathLen+1 > int(maxBuffer) {
		return 0, ErrBufferTooSmall
	}

	copy(pathBuf[:], path)
	pathBuf[pathLen] = 0

	if fsBufferWrite(uint32(uintptr(unsafe.Pointer(&pathBuf[0]))), uint32(pathLen+1), 0) != 0 {
		return 0, ErrHostCallFailed
	}
	return pathLen, ErrOK
}

type startupAPI struct{}

var emptyArgs = []string{}

func rawWriteString(s string) Error {
	if len(s) == 0 {
		return ErrOK
	}
	ptr := unsafe.StringData(s)
	if consoleWrite(uint32(uintptr(unsafe.Pointer(ptr))), uint32(len(s))) != 0 {
		return ErrHostCallFailed
	}
	return ErrOK
}

func rawWriteBytes(b []byte) Error {
	if len(b) == 0 {
		return ErrOK
	}
	if consoleWrite(uint32(uintptr(unsafe.Pointer(&b[0]))), uint32(len(b))) != 0 {
		return ErrHostCallFailed
	}
	return ErrOK
}

func (startupAPI) Arg(index int) int32 {
	if index < 0 || index >= len(startupArgs) {
		return 0
	}
	return startupArgs[index]
}

func main() {}

//export wasmos_main
func wasmos_main(arg0, arg1, arg2, arg3 int32) int32 {
	startupArgs[0] = arg0
	startupArgs[1] = arg1
	startupArgs[2] = arg2
	startupArgs[3] = arg3
	return Main(emptyArgs)
}

func ensureFSReplyEndpoint() (int32, Error) {
	if fsReplyEndpoint >= 0 {
		return fsReplyEndpoint, ErrOK
	}
	endpoint := ipcCreateEndpoint()
	if endpoint < 0 {
		return -1, ErrNotAvailable
	}
	fsReplyEndpoint = endpoint
	return endpoint, ErrOK
}

func nextFSRequestID() int32 {
	requestID := fsRequestID
	fsRequestID++
	if fsRequestID < 1 {
		fsRequestID = 1
	}
	return requestID
}

func fsRequest(msgType int32, arg0 int32, arg1 int32, arg2 int32, arg3 int32) (int32, int32, Error) {
	endpoint := fsEndpoint()
	if endpoint < 0 {
		return -1, 0, ErrNotAvailable
	}

	replyEndpoint, err := ensureFSReplyEndpoint()
	if err != ErrOK {
		return -1, 0, err
	}

	requestID := nextFSRequestID()
	if ipcSend(endpoint, replyEndpoint, msgType, requestID, arg0, arg1, arg2, arg3) != 0 {
		return -1, 0, ErrHostCallFailed
	}
	if ipcRecv(replyEndpoint) < 0 {
		return -1, 0, ErrHostCallFailed
	}
	if ipcLastField(ipcFieldRequestID) != requestID || ipcLastField(ipcFieldType) != fsIPCResp {
		return -1, 0, ErrBadResponse
	}
	return ipcLastField(ipcFieldArg0), ipcLastField(ipcFieldArg1), ErrOK
}

func (stdAPI) WriteString(s string) Error {
	return rawWriteString(s)
}

func (stdAPI) WriteBytes(b []byte) Error {
	return rawWriteBytes(b)
}

func (stdAPI) Puts(s string) Error {
	return rawWriteString(s)
}

func (stdAPI) Println(s string) Error {
	return rawWriteString(s + "\n")
}

func (stdAPI) Printf(s string) Error {
	return rawWriteString(s)
}

func (stdAPI) ReadLine(buffer []byte) (int, Error) {
	if len(buffer) <= 1 {
		return 0, ErrInvalidArgument
	}
	pos := 0
	for pos+1 < len(buffer) {
		got := consoleRead(uint32(uintptr(unsafe.Pointer(&buffer[pos]))), 1)
		if got < 0 {
			buffer[0] = 0
			return 0, ErrHostCallFailed
		}
		if got == 0 {
			break
		}
		pos++
		if buffer[pos-1] == '\n' {
			break
		}
	}
	buffer[pos] = 0
	return pos, ErrOK
}

func (File) Invalid() File {
	return File{fd: -1}
}

func (f File) Read(buffer []byte) (int, Error) {
	if len(buffer) == 0 {
		return 0, ErrOK
	}

	maxBuffer := fsBufferSize()
	if maxBuffer <= 0 {
		return 0, ErrNotAvailable
	}

	done := 0
	for done < len(buffer) {
		remaining := len(buffer) - done
		chunkLen := remaining
		if chunkLen > int(maxBuffer) {
			chunkLen = int(maxBuffer)
		}

		chunkRead, _, err := fsRequest(fsIPCReadReq, f.fd, int32(chunkLen), 0, 0)
		if err != ErrOK {
			return done, err
		}
		if chunkRead < 0 {
			return done, ErrBadResponse
		}
		if chunkRead == 0 {
			break
		}
		if chunkRead > maxBuffer || int(chunkRead) > chunkLen {
			return done, ErrBadResponse
		}
		if fsBufferCopy(uint32(uintptr(unsafe.Pointer(&buffer[done]))), uint32(chunkRead), 0) != 0 {
			return done, ErrHostCallFailed
		}
		done += int(chunkRead)
		if int(chunkRead) != chunkLen {
			break
		}
	}

	return done, ErrOK
}

func (f File) Close() Error {
	_, _, err := fsRequest(fsIPCCloseReq, f.fd, 0, 0, 0)
	return err
}

func (f File) Write(buffer []byte) (int, Error) {
	if len(buffer) == 0 {
		return 0, ErrOK
	}

	maxBuffer := fsBufferSize()
	if maxBuffer <= 0 {
		return 0, ErrNotAvailable
	}

	done := 0
	for done < len(buffer) {
		chunkLen := len(buffer) - done
		if chunkLen > int(maxBuffer) {
			chunkLen = int(maxBuffer)
		}
		if fsBufferWrite(uint32(uintptr(unsafe.Pointer(&buffer[done]))), uint32(chunkLen), 0) != 0 {
			return done, ErrHostCallFailed
		}
		chunkWritten, _, err := fsRequest(fsIPCWriteReq, f.fd, int32(chunkLen), 0, 0)
		if err != ErrOK {
			return done, err
		}
		if chunkWritten < 0 || int(chunkWritten) > chunkLen {
			return done, ErrBadResponse
		}
		done += int(chunkWritten)
		if chunkWritten == 0 || int(chunkWritten) != chunkLen {
			break
		}
	}

	return done, ErrOK
}

func (f File) Seek(offset int32, whence int32) (int32, Error) {
	position, _, err := fsRequest(fsIPCSeekReq, f.fd, offset, whence, 0)
	if err != ErrOK {
		return -1, err
	}
	if position < 0 {
		return -1, ErrBadResponse
	}
	return position, ErrOK
}

func (fsAPI) openWithFlags(path string, flags int32) (File, Error) {
	pathLen, err := stagePath(path)
	if err != ErrOK {
		return File{fd: -1}, err
	}

	fd, _, err := fsRequest(fsIPCOpenReq, int32(pathLen), flags, 0, 0)
	if err != ErrOK {
		return File{fd: -1}, err
	}
	if fd < 0 {
		return File{fd: -1}, ErrBadResponse
	}
	return File{fd: fd}, ErrOK
}

func (api fsAPI) OpenRead(path string) (File, Error) {
	return api.openWithFlags(path, O_RDONLY)
}

func (api fsAPI) OpenWrite(path string) (File, Error) {
	return api.openWithFlags(path, O_WRONLY)
}

func (api fsAPI) Create(path string) (File, Error) {
	return api.openWithFlags(path, O_WRONLY|O_CREAT|O_TRUNC)
}

func (api fsAPI) OpenAppend(path string) (File, Error) {
	return api.openWithFlags(path, O_WRONLY|O_CREAT|O_APPEND)
}

func (fsAPI) Stat(path string) (FileStat, Error) {
	pathLen, err := stagePath(path)
	if err != ErrOK {
		return FileStat{}, err
	}

	size, mode, err := fsRequest(fsIPCStatReq, int32(pathLen), 0, 0, 0)
	if err != ErrOK {
		return FileStat{}, err
	}
	if size < 0 {
		return FileStat{}, ErrBadResponse
	}
	return FileStat{Size: size, Mode: mode & (SIFREG | SIFDIR)}, ErrOK
}

func (fsAPI) Unlink(path string) Error {
	pathLen, err := stagePath(path)
	if err != ErrOK {
		return err
	}
	_, _, err = fsRequest(fsIPCUnlinkReq, int32(pathLen), 0, 0, 0)
	return err
}

func (fsAPI) Mkdir(path string) Error {
	pathLen, err := stagePath(path)
	if err != ErrOK {
		return err
	}
	_, _, err = fsRequest(fsIPCMkdirReq, int32(pathLen), 0, 0, 0)
	return err
}

func (fsAPI) Rmdir(path string) Error {
	pathLen, err := stagePath(path)
	if err != ErrOK {
		return err
	}
	_, _, err = fsRequest(fsIPCRmdirReq, int32(pathLen), 0, 0, 0)
	return err
}
