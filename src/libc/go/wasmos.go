package main

import "unsafe"

const (
	fsIPCOpenReq    int32 = 0x400
	fsIPCReadReq    int32 = 0x401
	fsIPCWriteReq   int32 = 0x406
	fsIPCCloseReq   int32 = 0x402
	fsIPCStatReq    int32 = 0x403
	fsIPCSeekReq    int32 = 0x405
	fsIPCUnlinkReq  int32 = 0x407
	fsIPCMkdirReq   int32 = 0x408
	fsIPCRmdirReq   int32 = 0x409
	fsIPCReaddirReq int32 = 0x410
	fsIPCResp       int32 = 0x480
	fsIPCStream     int32 = 0x481
)

const (
	ipcFieldType        int32 = 0
	ipcFieldRequestID   int32 = 1
	ipcFieldArg0        int32 = 2
	ipcFieldArg1        int32 = 3
	ipcFieldSource      int32 = 4
	ipcFieldDestination int32 = 5
	ipcFieldArg2        int32 = 6
	ipcFieldArg3        int32 = 7
)

// SeekSet, SeekCur and SeekEnd are the whence values for File.Seek: the new
// offset is measured from the start of the file, from the current offset, or
// from the file size. The FS backend refuses a resulting offset outside
// [0, size], so seeking past the end fails instead of extending the file.
//
// SIFREG and SIFDIR are the file-type bits of FileStat.Mode; Stat masks off
// everything else, and the FS reply carries no permission bits.
//
// O_RDONLY..O_TRUNC are POSIX-valued open flags. Bit 0 is the access mode
// (O_RDONLY or O_WRONLY); the rest are modifiers the FS backend accepts only
// alongside O_WRONLY. There is no read/write mode: any other bit is rejected.
const (
	SeekSet     int32 = 0
	SeekCur     int32 = 1
	SeekEnd     int32 = 2
	SIFREG      int32 = 0x8000
	SIFDIR      int32 = 0x4000
	O_RDONLY    int32 = 0
	O_WRONLY    int32 = 1
	O_APPEND    int32 = 0x0008
	O_CREAT     int32 = 0x0040
	O_TRUNC     int32 = 0x0200
	xferGrantRW int32 = 0x3
)

// Error is this port's own failure taxonomy, not a packed abi/errors.yaml code:
// the filesystem protocol's WASMOS_ERR_FS_* status is not surfaced, because a
// request the backend refuses comes back as an FS error message that every entry
// point here reports as ErrBadResponse.
type Error int32

// Error values. ErrOK is success and is what a call returns when it worked; the
// rest are the failure modes:
//
//	ErrBadResponse     a reply arrived but did not match the request (wrong
//	                   message type, wrong request id, or a negative status)
//	ErrBufferTooSmall  the path plus its NUL does not fit the transfer buffer
//	ErrHostCallFailed  a host call refused the operation
//	ErrInvalidArgument an empty path, or a ReadLine buffer under two bytes
//	ErrNameTooLong     the path plus its NUL exceeds the 256-byte staging buffer
//	ErrNotAvailable    no FS service, no endpoint, or no transfer buffer
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

//go:wasmimport wasmos proc_exit
func procExit(status int32) int32

//go:wasmimport wasmos ipc_create_endpoint
func ipcCreateEndpoint() int32

//go:wasmimport wasmos ipc_send
func ipcSend(destinationEndpoint int32, sourceEndpoint int32, msgType int32, requestID int32, arg0 int32, arg1 int32, arg2 int32, arg3 int32) int32

//go:wasmimport wasmos ipc_select_one
func ipcRecv(endpoint int32) int32

//go:wasmimport wasmos ipc_last_field
func ipcLastField(field int32) int32

//go:wasmimport wasmos fs_endpoint
func fsEndpoint() int32

//go:wasmimport wasmos xfer_buffer_size
func fsBufferSize() int32

//go:wasmimport wasmos xfer_buffer_acquire
func xferBufferAcquire(minimumSize int32) int32

//go:wasmimport wasmos xfer_buffer_borrow
func xferBufferBorrow(granteeEndpoint int32, bufferID int32, flags int32) int32

//go:wasmimport wasmos xfer_buffer_release
func xferBufferRelease(bufferID int32) int32

//go:wasmimport wasmos xfer_buffer_write
func fsBufferWrite(bufferID int32, ptr uint32, len uint32, offset uint32) int32

//go:wasmimport wasmos xfer_buffer_read
func fsBufferCopy(bufferID int32, ptr uint32, len uint32, offset uint32) int32

//go:wasmimport wasmos thread_gettid
func threadGetTid() int32

//go:wasmimport wasmos thread_yield
func threadYield() int32

//go:wasmimport wasmos mutex_try_lock
func mutexTryLock(ptr uint32) int32

//go:wasmimport wasmos mutex_unlock
func mutexUnlock(ptr uint32) int32

var fsReplyEndpoint int32 = -1
var fsRequestID int32 = 1
var ipcReplyEndpoint int32 = -1
var ipcRequestID int32 = 1
var startupArgs [4]int32
var startup = startupAPI{}

type stdAPI struct{}

var std = stdAPI{}

// IPCReply is a received message, copied out of the caller's last-received
// slot. The four argument words are protocol-defined; Source is the endpoint to
// address a reply to, Destination the endpoint it arrived on.
type IPCReply struct {
	Type        int32
	RequestID   int32
	Source      int32
	Destination int32
	Arg0        int32
	Arg1        int32
	Arg2        int32
	Arg3        int32
}

type ipcAPI struct{}

var ipc = ipcAPI{}

// Mutex is a recursive mutex whose state lives in guest memory and whose
// arbitration is done by the kernel, layout-compatible with wasmos_mutex_t.
// OwnerTID is the thread id of the current owner (0 when unlocked) and
// RecursionDepth the number of unmatched acquisitions it holds; both are written
// by the kernel.
//
// The kernel link tables export no wasmos.mutex_try_lock or mutex_unlock
// (FIXME(user-mutex-import) in src/libc/include/wasmos/api.h), so a module that
// actually calls these fails to instantiate on an unresolved import.
type Mutex struct {
	OwnerTID       uint32
	RecursionDepth uint32
}

// Init resets the mutex to the unlocked state, and does nothing on a nil
// receiver. Zeroing a mutex another thread holds loses that ownership, so only
// init one nobody has locked.
func (m *Mutex) Init() {
	if m == nil {
		return
	}
	m.OwnerTID = 0
	m.RecursionDepth = 0
}

// CurrentTID returns the calling thread's id, as the kernel records it in
// Mutex.OwnerTID.
func CurrentTID() int32 {
	return threadGetTid()
}

// TryLock makes one acquisition attempt without blocking: 0 when the mutex is
// now held by this thread (raising RecursionDepth if it already was), 1 when
// another thread owns it, negative on error or a nil receiver.
func (m *Mutex) TryLock() int32 {
	if m == nil {
		return -1
	}
	return mutexTryLock(uint32(uintptr(unsafe.Pointer(m))))
}

// Lock acquires the mutex, yielding the thread between attempts while another
// owner holds it. It returns 0 once held, or the negative code that ended the
// retry loop. This is a yield-spin, not a sleep.
func (m *Mutex) Lock() int32 {
	if m == nil {
		return -1
	}
	for {
		rc := m.TryLock()
		if rc != 1 {
			return rc
		}
		threadYield()
	}
}

// Unlock drops one acquisition, releasing the mutex when RecursionDepth reaches
// zero. It returns 0 on success, negative when the caller is not the owner or
// the receiver is nil.
func (m *Mutex) Unlock() int32 {
	if m == nil {
		return -1
	}
	return mutexUnlock(uint32(uintptr(unsafe.Pointer(m))))
}

func ensureIPCReplyEndpoint() (int32, Error) {
	if ipcReplyEndpoint >= 0 {
		return ipcReplyEndpoint, ErrOK
	}
	ep := ipcCreateEndpoint()
	if ep < 0 {
		return -1, ErrNotAvailable
	}
	ipcReplyEndpoint = ep
	return ep, ErrOK
}

func nextIPCRequestID() int32 {
	id := ipcRequestID
	ipcRequestID++
	if ipcRequestID < 1 {
		ipcRequestID = 1
	}
	return id
}

func readIPCReply() IPCReply {
	return IPCReply{
		Type:        ipcLastField(ipcFieldType),
		RequestID:   ipcLastField(ipcFieldRequestID),
		Source:      ipcLastField(ipcFieldSource),
		Destination: ipcLastField(ipcFieldDestination),
		Arg0:        ipcLastField(ipcFieldArg0),
		Arg1:        ipcLastField(ipcFieldArg1),
		Arg2:        ipcLastField(ipcFieldArg2),
		Arg3:        ipcLastField(ipcFieldArg3),
	}
}

// Call sends a request to server and blocks until the FIRST message arrives on
// the per-context managed reply endpoint; it is returned as the reply without
// checking its request id or source. Only one request may be outstanding on that
// endpoint at a time, or a stale reply is returned for a later call. The C
// helper (wasmos_ipc_call) matches instead.
func (ipcAPI) Call(server int32, msgType int32, arg0 int32, arg1 int32, arg2 int32, arg3 int32) (IPCReply, Error) {
	replyEp, err := ensureIPCReplyEndpoint()
	if err != ErrOK {
		return IPCReply{}, err
	}
	requestID := nextIPCRequestID()
	if ipcSend(server, replyEp, msgType, requestID, arg0, arg1, arg2, arg3) != 0 {
		return IPCReply{}, ErrHostCallFailed
	}
	if ipcRecv(replyEp) < 0 {
		return IPCReply{}, ErrHostCallFailed
	}
	return readIPCReply(), ErrOK
}

// Recv blocks until a message arrives on endpoint (for servers).
// It parks the process indefinitely: no timeout, no interruption. Every message
// queued on endpoint is returned, replies and requests alike, so a server that
// also issues requests must demultiplex on RequestID itself. ErrHostCallFailed
// on an invalid endpoint or a receive error.
func (ipcAPI) Recv(endpoint int32) (IPCReply, Error) {
	if ipcRecv(endpoint) < 0 {
		return IPCReply{}, ErrHostCallFailed
	}
	return readIPCReply(), ErrOK
}

// Reply sends a reply from a server back to the caller's private reply endpoint.
// source should be the server's own service endpoint.
// destination should be req.Source from the incoming request.
// requestID must be echoed from the request or the caller cannot match the
// reply. It returns once the message is queued, not once the peer has read it;
// ErrHostCallFailed means the send itself was refused.
func (ipcAPI) Reply(destination int32, source int32, msgType int32, requestID int32, arg0 int32, arg1 int32, arg2 int32, arg3 int32) Error {
	if ipcSend(destination, source, msgType, requestID, arg0, arg1, arg2, arg3) != 0 {
		return ErrHostCallFailed
	}
	return ErrOK
}

// CreateEndpoint allocates a new message endpoint (for servers).
func (ipcAPI) CreateEndpoint() (int32, Error) {
	ep := ipcCreateEndpoint()
	if ep < 0 {
		return -1, ErrNotAvailable
	}
	return ep, ErrOK
}

// File is an open file, holding the FS manager's client-side descriptor. It is a
// value type: copying it copies the descriptor, and only Close releases it.
type File struct {
	fd int32
}

// FileStat is the result of Stat: Size is the file length in bytes, Mode carries
// only the SIFREG / SIFDIR type bits.
type FileStat struct {
	Size int32
	Mode int32
}

type fsAPI struct{}

var fs = fsAPI{}

// RequestAsync submits one filesystem protocol request without blocking. The
// caller owns request and any transfer buffer referenced by arg2/arg3 until
// the returned future settles; replyEndpoint must be the EventLoop endpoint.
func (fsAPI) RequestAsync(loop *EventLoop, request *FSRequest, replyEndpoint int32, msgType int32, arg0 int32, arg1 int32, arg2 int32, arg3 int32) (*Future, int32, Error) {
	endpoint := fsEndpoint()
	if endpoint < 0 || replyEndpoint < 0 || loop == nil || request == nil {
		return nil, 0, ErrNotAvailable
	}
	request.Init()
	future, requestID := request.Send(loop, endpoint, replyEndpoint, msgType, arg0, arg1, arg2, arg3)
	if future == nil {
		return nil, 0, ErrHostCallFailed
	}
	return future, requestID, ErrOK
}

type stagedPath struct {
	bid     int32
	b1      int32
	pathLen int
}

type borrowedBuffer struct {
	bid int32
	b1  int32
}

func borrowFSBuffer(size int32) (borrowedBuffer, Error) {
	bid := xferBufferAcquire(size)
	if bid < 0 {
		return borrowedBuffer{}, ErrNotAvailable
	}
	b1 := xferBufferBorrow(fsEndpoint(), bid, xferGrantRW)
	if b1 < 0 {
		_ = xferBufferRelease(bid)
		return borrowedBuffer{}, ErrHostCallFailed
	}
	return borrowedBuffer{bid: bid, b1: b1}, ErrOK
}

func stagePath(path string) (stagedPath, Error) {
	pathLen := len(path)
	maxBuffer := fsBufferSize()
	var pathBuf [256]byte

	if pathLen == 0 {
		return stagedPath{}, ErrInvalidArgument
	}
	if maxBuffer <= 0 {
		return stagedPath{}, ErrNotAvailable
	}
	if pathLen+1 > len(pathBuf) {
		return stagedPath{}, ErrNameTooLong
	}
	if pathLen+1 > int(maxBuffer) {
		return stagedPath{}, ErrBufferTooSmall
	}

	copy(pathBuf[:], path)
	pathBuf[pathLen] = 0

	xfer, err := borrowFSBuffer(int32(pathLen + 1))
	if err != ErrOK {
		return stagedPath{}, err
	}
	if fsBufferWrite(xfer.bid, uint32(uintptr(unsafe.Pointer(&pathBuf[0]))), uint32(pathLen+1), 0) != 0 {
		_ = xferBufferRelease(xfer.bid)
		return stagedPath{}, ErrHostCallFailed
	}
	return stagedPath{bid: xfer.bid, b1: xfer.b1, pathLen: pathLen}, ErrOK
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

// Arg returns one of the four wasmos_main entry-arg registers, as received.
//
// FIXME(spawn-info): PM retired the entry-arg bindings and always passes zeros
// (pm_apply_entry_bindings in process_manager_spawn.c), so every index reads 0
// here. The C, Zig and AssemblyScript ports instead read the spawn-info buffer
// (wasmos_spawn_info.h) via the spawn_info_buffer host call, where index 0 means
// proc.endpoint, and expose tty/module count+index and the argv blob alongside
// it; this port has none of that, so a Go guest cannot reach its process manager
// endpoint or its argv (Main is likewise always handed an empty slice).
func (startupAPI) Arg(index int) int32 {
	if index < 0 || index >= len(startupArgs) {
		return 0
	}
	return startupArgs[index]
}

// main exists only to satisfy the Go toolchain: a WASMOS guest is entered
// through the wasmos_main export below, never through this function.
func main() {}

// wasmos_main is the entry point the process manager calls instead of _start. It
// records the four entry-arg registers for startup.Arg, calls the application's
// Main with an empty argument slice (this port does not read the spawn-info argv
// blob), and reports Main's return value to the process manager. proc_exit does
// not return, so the trailing return is unreachable in a live process.
//
//export wasmos_main
func wasmos_main(arg0, arg1, arg2, arg3 int32) int32 {
	startupArgs[0] = arg0
	startupArgs[1] = arg1
	startupArgs[2] = arg2
	startupArgs[3] = arg3
	rc := Main(emptyArgs)
	procExit(rc)
	return rc
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

func fsRequestStream(msgType int32, arg0 int32, arg1 int32, arg2 int32, arg3 int32, out []byte) (int, Error) {
	endpoint := fsEndpoint()
	if endpoint < 0 || len(out) == 0 {
		return 0, ErrNotAvailable
	}
	replyEndpoint, err := ensureFSReplyEndpoint()
	if err != ErrOK {
		return 0, err
	}
	requestID := nextFSRequestID()
	if ipcSend(endpoint, replyEndpoint, msgType, requestID, arg0, arg1, arg2, arg3) != 0 {
		return 0, ErrHostCallFailed
	}
	outLen := 0
	for {
		if ipcRecv(replyEndpoint) < 0 {
			return 0, ErrHostCallFailed
		}
		if ipcLastField(ipcFieldRequestID) != requestID {
			continue
		}
		t := ipcLastField(ipcFieldType)
		if t == fsIPCStream {
			args := [4]int32{
				ipcLastField(ipcFieldArg0),
				ipcLastField(ipcFieldArg1),
				ipcLastField(ipcFieldArg2),
				ipcLastField(ipcFieldArg3),
			}
			for i := 0; i < 4; i++ {
				c := byte(args[i] & 0xFF)
				if c == 0 {
					continue
				}
				if outLen+1 >= len(out) {
					out[len(out)-1] = 0
					return outLen, ErrOK
				}
				out[outLen] = c
				outLen++
			}
			continue
		}
		if t != fsIPCResp || ipcLastField(ipcFieldArg0) != 0 {
			return 0, ErrBadResponse
		}
		if outLen < len(out) {
			out[outLen] = 0
		}
		return outLen, ErrOK
	}
}

// WriteString writes s verbatim to the console. No newline is added, an empty
// string is a no-op success, and ErrHostCallFailed means the console write was
// refused.
func (stdAPI) WriteString(s string) Error {
	return rawWriteString(s)
}

// WriteBytes writes b verbatim to the console; see WriteString.
func (stdAPI) WriteBytes(b []byte) Error {
	return rawWriteBytes(b)
}

// Puts is identical to WriteString: the trailing newline C's puts adds is not
// added here.
func (stdAPI) Puts(s string) Error {
	return rawWriteString(s)
}

// Println writes s followed by a newline. The concatenation allocates, so a hot
// loop is better served by WriteString.
func (stdAPI) Println(s string) Error {
	return rawWriteString(s + "\n")
}

// Printf writes s verbatim: this port does no formatting and takes no format
// arguments. The name is kept for parity with the other language ports; use
// Go's own string formatting to build s.
func (stdAPI) Printf(s string) Error {
	return rawWriteString(s)
}

// ReadLine reads console bytes into buffer up to and including the first
// newline, NUL-terminates them, and returns the byte count excluding the NUL.
//
// The newline, when one arrived, is part of the count. Reads stop as soon as the
// console has no byte ready, so a short return is normal and does not mean end
// of input; this does not park until a full line exists. A buffer shorter than
// two bytes is ErrInvalidArgument, a full buffer ends the read with the
// terminator in the last byte, and a host-call failure clears buffer[0] and
// returns (0, ErrHostCallFailed).
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

// Invalid returns a File carrying the sentinel descriptor -1, the value the
// open helpers return alongside a failure. The receiver is ignored, so it can be
// called on any File value.
func (File) Invalid() File {
	return File{fd: -1}
}

// Read reads up to len(buffer) bytes at the file's current offset and returns
// how many were stored.
//
// It loops over transfer-buffer-sized chunks, so a short reply ends the read: 0
// means end of file, and a value below len(buffer) is not an error. An empty
// buffer is a 0-byte success that issues no request. On failure the count read
// so far is returned along with the error, and the transfer buffer is released
// on every path, which also revokes the FS manager's borrow.
func (f File) Read(buffer []byte) (int, Error) {
	if len(buffer) == 0 {
		return 0, ErrOK
	}

	maxBuffer := fsBufferSize()
	if maxBuffer <= 0 {
		return 0, ErrNotAvailable
	}
	xfer, err := borrowFSBuffer(maxBuffer)
	if err != ErrOK {
		return 0, err
	}
	defer xferBufferRelease(xfer.bid)

	done := 0
	for done < len(buffer) {
		remaining := len(buffer) - done
		chunkLen := remaining
		if chunkLen > int(maxBuffer) {
			chunkLen = int(maxBuffer)
		}

		chunkRead, _, err := fsRequest(fsIPCReadReq, f.fd, int32(chunkLen), xfer.bid, xfer.b1)
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
		if fsBufferCopy(xfer.bid, uint32(uintptr(unsafe.Pointer(&buffer[done]))), uint32(chunkRead), 0) != 0 {
			return done, ErrHostCallFailed
		}
		done += int(chunkRead)
		if int(chunkRead) != chunkLen {
			break
		}
	}

	return done, ErrOK
}

// Close releases the descriptor at the FS manager. A refusal surfaces as
// ErrBadResponse and the status word of a successful response is ignored; the
// File value itself is unchanged, so closing twice sends a second request.
func (f File) Close() Error {
	_, _, err := fsRequest(fsIPCCloseReq, f.fd, 0, 0, 0)
	return err
}

// Write writes buffer at the file's current offset and returns how many bytes
// the FS manager accepted.
//
// Chunked like Read: a chunk only partially accepted ends the loop, so a short
// return is a real short write rather than an error. An empty buffer is a 0-byte
// success that issues no request.
func (f File) Write(buffer []byte) (int, Error) {
	if len(buffer) == 0 {
		return 0, ErrOK
	}

	maxBuffer := fsBufferSize()
	if maxBuffer <= 0 {
		return 0, ErrNotAvailable
	}
	xfer, err := borrowFSBuffer(maxBuffer)
	if err != ErrOK {
		return 0, err
	}
	defer xferBufferRelease(xfer.bid)

	done := 0
	for done < len(buffer) {
		chunkLen := len(buffer) - done
		if chunkLen > int(maxBuffer) {
			chunkLen = int(maxBuffer)
		}
		if fsBufferWrite(xfer.bid, uint32(uintptr(unsafe.Pointer(&buffer[done]))), uint32(chunkLen), 0) != 0 {
			return done, ErrHostCallFailed
		}
		chunkWritten, _, err := fsRequest(fsIPCWriteReq, f.fd, int32(chunkLen), xfer.bid, xfer.b1)
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

// Seek moves the file offset to offset bytes from the SeekSet / SeekCur /
// SeekEnd origin and returns the new absolute offset. A target outside
// [0, size] is refused by the backend and surfaces as (-1, ErrBadResponse);
// seeking past the end does not extend the file.
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
	staged, err := stagePath(path)
	if err != ErrOK {
		return File{fd: -1}, err
	}
	defer xferBufferRelease(staged.bid)

	fd, _, err := fsRequest(fsIPCOpenReq, int32(staged.pathLen), flags, staged.bid, staged.b1)
	if err != ErrOK {
		return File{fd: -1}, err
	}
	if fd < 0 {
		return File{fd: -1}, ErrBadResponse
	}
	return File{fd: fd}, ErrOK
}

// OpenRead opens an existing file for reading. path must be non-empty and
// shorter than 256 bytes including its NUL. A missing file yields
// (Invalid, ErrBadResponse).
func (api fsAPI) OpenRead(path string) (File, Error) {
	return api.openWithFlags(path, O_RDONLY)
}

// OpenWrite opens an existing file for writing at offset 0 without truncating
// it. It does not create the file.
func (api fsAPI) OpenWrite(path string) (File, Error) {
	return api.openWithFlags(path, O_WRONLY)
}

// Create opens path for writing, creating it if needed and truncating it to zero
// length.
func (api fsAPI) Create(path string) (File, Error) {
	return api.openWithFlags(path, O_WRONLY|O_CREAT|O_TRUNC)
}

// OpenAppend opens path for writing, creating it if needed and positioning
// writes at the end.
func (api fsAPI) OpenAppend(path string) (File, Error) {
	return api.openWithFlags(path, O_WRONLY|O_CREAT|O_APPEND)
}

// Stat returns the size and file-type bits of path without opening it. A missing
// path yields ErrBadResponse.
func (fsAPI) Stat(path string) (FileStat, Error) {
	staged, err := stagePath(path)
	if err != ErrOK {
		return FileStat{}, err
	}
	defer xferBufferRelease(staged.bid)

	size, mode, err := fsRequest(fsIPCStatReq, int32(staged.pathLen), 0, staged.bid, staged.b1)
	if err != ErrOK {
		return FileStat{}, err
	}
	if size < 0 {
		return FileStat{}, ErrBadResponse
	}
	return FileStat{Size: size, Mode: mode & (SIFREG | SIFDIR)}, ErrOK
}

// Unlink removes a file. A refusal by the backend (missing file, directory,
// read-only mount) arrives as an FS error message and surfaces as
// ErrBadResponse; the status word of a successful response is not inspected.
func (fsAPI) Unlink(path string) Error {
	staged, err := stagePath(path)
	if err != ErrOK {
		return err
	}
	defer xferBufferRelease(staged.bid)
	_, _, err = fsRequest(fsIPCUnlinkReq, int32(staged.pathLen), 0, staged.bid, staged.b1)
	return err
}

// Mkdir creates a directory. It reports failure the same way as Unlink.
func (fsAPI) Mkdir(path string) Error {
	staged, err := stagePath(path)
	if err != ErrOK {
		return err
	}
	defer xferBufferRelease(staged.bid)
	_, _, err = fsRequest(fsIPCMkdirReq, int32(staged.pathLen), 0, staged.bid, staged.b1)
	return err
}

// Rmdir removes a directory. It reports failure the same way as Unlink.
func (fsAPI) Rmdir(path string) Error {
	staged, err := stagePath(path)
	if err != ErrOK {
		return err
	}
	defer xferBufferRelease(staged.bid)
	_, _, err = fsRequest(fsIPCRmdirReq, int32(staged.pathLen), 0, staged.bid, staged.b1)
	return err
}

// ReadDir lists the current directory into buffer as a NUL-terminated text blob
// and returns its length excluding the terminator.
//
// The listing arrives as a stream of messages carrying four bytes each, and zero
// bytes inside a message are skipped rather than stored. A buffer too small is
// filled, terminated and returned truncated -- the remaining stream messages
// keep arriving on the reply endpoint and are not drained.
func (fsAPI) ReadDir(buffer []byte) (int, Error) {
	return fsRequestStream(fsIPCReaddirReq, 0, 0, 0, 0, buffer)
}
