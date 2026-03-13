package main

import "unsafe"

const (
	fsIPCOpenReq  int32 = 0x400
	fsIPCReadReq  int32 = 0x401
	fsIPCCloseReq int32 = 0x402
	fsIPCResp     int32 = 0x480
)

const (
	ipcFieldType      int32 = 0
	ipcFieldRequestID int32 = 1
	ipcFieldArg0      int32 = 2
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

type stdAPI struct{}

var std = stdAPI{}

type File struct {
	fd int32
}

type fsAPI struct{}

var fs = fsAPI{}

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

func fsRequest(msgType int32, arg0 int32, arg1 int32, arg2 int32, arg3 int32) (int32, Error) {
	endpoint := fsEndpoint()
	if endpoint < 0 {
		return -1, ErrNotAvailable
	}

	replyEndpoint, err := ensureFSReplyEndpoint()
	if err != ErrOK {
		return -1, err
	}

	requestID := nextFSRequestID()
	if ipcSend(endpoint, replyEndpoint, msgType, requestID, arg0, arg1, arg2, arg3) != 0 {
		return -1, ErrHostCallFailed
	}
	if ipcRecv(replyEndpoint) < 0 {
		return -1, ErrHostCallFailed
	}
	if ipcLastField(ipcFieldRequestID) != requestID || ipcLastField(ipcFieldType) != fsIPCResp {
		return -1, ErrBadResponse
	}
	return ipcLastField(ipcFieldArg0), ErrOK
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

		chunkRead, err := fsRequest(fsIPCReadReq, f.fd, int32(chunkLen), 0, 0)
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
	_, err := fsRequest(fsIPCCloseReq, f.fd, 0, 0, 0)
	return err
}

func (fsAPI) OpenRead(path string) (File, Error) {
	pathLen := len(path)
	maxBuffer := fsBufferSize()
	var pathBuf [256]byte

	if pathLen == 0 {
		return File{fd: -1}, ErrInvalidArgument
	}
	if maxBuffer <= 0 {
		return File{fd: -1}, ErrNotAvailable
	}
	if pathLen+1 > len(pathBuf) {
		return File{fd: -1}, ErrNameTooLong
	}
	if pathLen+1 > int(maxBuffer) {
		return File{fd: -1}, ErrBufferTooSmall
	}

	copy(pathBuf[:], path)
	pathBuf[pathLen] = 0

	if fsBufferWrite(uint32(uintptr(unsafe.Pointer(&pathBuf[0]))), uint32(pathLen+1), 0) != 0 {
		return File{fd: -1}, ErrHostCallFailed
	}

	fd, err := fsRequest(fsIPCOpenReq, int32(pathLen), 0, 0, 0)
	if err != ErrOK {
		return File{fd: -1}, err
	}
	if fd < 0 {
		return File{fd: -1}, ErrBadResponse
	}
	return File{fd: fd}, ErrOK
}
