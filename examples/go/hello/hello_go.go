// WASMOS "hello world" for the Go (TinyGo) guest binding.
//
// Demonstrates console output (std.Puts / std.Printf) and the async filesystem
// API, which is the part worth studying. Each fs.*Async call returns an
// operation whose Then chains the next step and yields a *Future; returning
// that future is what schedules the step. RunAsyncApp drives the chain and
// returns the app's exit status, and returning nil ends it — which is what the
// fail* helpers do after printing their diagnostic.
//
// Unlike the Rust and Zig ports, Go closures can capture, so the fd is carried
// in a closure rather than a global; only the read buffers are package state,
// to keep them off the small guest stack.
//
// The chain reads one byte of /boot/startup.nsh, then creates a file with a
// long (non-8.3) name, writes it, reads it back for comparison, unlinks it, and
// stats the removed path expecting a rejection — CatchGo turns that rejection
// into the success report, and it needs a caller-provided Continuation because
// the runtime does not allocate one.
//
// Preconditions: /boot/startup.nsh must exist and be at least one byte, and the
// working directory must be writable — the test file uses a relative path.
package main

const helloPath = "go-long-file-check.txt"

var helloData struct {
	startup [1]byte
	check   [32]byte
	content []byte
}

func failStartup(*AsyncFSOperation) *Future {
	_ = std.Puts("startup.nsh readable: false\n")
	return nil
}

func failWrite(*AsyncFSOperation) *Future {
	_ = std.Puts("long filename write: false\n")
	return nil
}

func failUnlink(*AsyncFSOperation) *Future {
	_ = std.Puts("long filename unlink: false\n")
	return nil
}

func startupOpened(open *AsyncFSOperation) *Future {
	fd := open.Result()
	if fd < 0 {
		return failStartup(open)
	}
	return fs.ReadAsync(fd, helloData.startup[:]).Then(func(read *AsyncFSOperation) *Future {
		if read.Result() != 1 {
			return failStartup(read)
		}
		return fs.CloseAsync(fd).Then(startupClosed)
	})
}

func startupClosed(close *AsyncFSOperation) *Future {
	if close.Result() < 0 {
		return failStartup(close)
	}
	_ = std.Puts("startup.nsh readable: true\n")
	return fs.OpenAsync(helloPath, O_WRONLY|O_CREAT|O_TRUNC).Then(fileCreated)
}

func fileCreated(create *AsyncFSOperation) *Future {
	fd := create.Result()
	if fd < 0 {
		return failWrite(create)
	}
	return fs.WriteAsync(fd, helloData.content).Then(func(write *AsyncFSOperation) *Future {
		if write.Result() != int32(len(helloData.content)) {
			return failWrite(write)
		}
		return fs.CloseAsync(fd).Then(writeClosed)
	})
}

func writeClosed(close *AsyncFSOperation) *Future {
	if close.Result() < 0 {
		return failWrite(close)
	}
	return fs.OpenAsync(helloPath, O_RDONLY).Then(verifyOpened)
}

func verifyOpened(open *AsyncFSOperation) *Future {
	fd := open.Result()
	if fd < 0 {
		return failWrite(open)
	}
	return fs.ReadAsync(fd, helloData.check[:]).Then(func(read *AsyncFSOperation) *Future {
		if read.Result() != int32(len(helloData.content)) || string(helloData.check[:len(helloData.content)]) != string(helloData.content) {
			return failWrite(read)
		}
		return fs.CloseAsync(fd).Then(verifyClosed)
	})
}

func verifyClosed(close *AsyncFSOperation) *Future {
	if close.Result() < 0 {
		return failWrite(close)
	}
	return fs.UnlinkAsync(helloPath).Then(fileUnlinked)
}

func fileUnlinked(unlink *AsyncFSOperation) *Future {
	if unlink.Result() < 0 {
		return failUnlink(unlink)
	}
	var statContinuation Continuation
	return fs.StatAsync(helloPath).future.CatchGo(AsyncAppRuntime(), &statContinuation, func(_ int32) (uintptr, int32) {
		_ = std.Puts("long filename write: true\n")
		_ = std.Puts("long filename unlink: true\n")
		return 0, 0
	})
}

// Main is the app entry point the Go runtime shim calls; args carries the
// spawn arguments and is unused here. It prints the banner and returns
// RunAsyncApp's status, which is 0 for a chain that ran to completion — the
// per-step outcomes are reported through the printed lines the boot test
// matches on, not through this value.
func Main(args []string) int32 {
	_ = args
	helloData.content = []byte("go shim long filename\n")
	_ = std.Puts("Hello from Go on WASMOS!\n")
	_ = std.Puts("This is a tiny WASMOS-APP written in Go.\n")
	_ = std.Printf("Entry: main\n")
	return RunAsyncApp(func() *Future {
		return fs.OpenAsync("/boot/startup.nsh", O_RDONLY).Then(startupOpened)
	})
}
