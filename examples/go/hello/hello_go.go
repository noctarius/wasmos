package main

func Main(args []string) int32 {
	_ = args
	const path = "/go-long-file-check.txt"
	content := []byte("go shim long filename\n")
	_ = std.Puts("Hello from Go on WASMOS!\n")
	_ = std.Puts("This is a tiny WASMOS-APP written in Go.\n")
	_ = std.Printf("Entry: main\n")

	file, err := fs.OpenRead("/startup.nsh")
	if err != ErrOK {
		_ = std.Puts("startup.nsh readable: false\n")
		return 0
	}

	var buf [1]byte
	n, readErr := file.Read(buf[:])
	_ = file.Close()
	if readErr != ErrOK || n <= 0 {
		_ = std.Puts("startup.nsh readable: false\n")
		return 0
	}

	_ = std.Puts("startup.nsh readable: true\n")

	out, err := fs.Create(path)
	if err != ErrOK {
		_ = std.Puts("long filename write: false\n")
		return 0
	}
	written, writeErr := out.Write(content)
	_ = out.Close()
	if writeErr != ErrOK || written != len(content) {
		_ = std.Puts("long filename write: false\n")
		return 0
	}

	verify, err := fs.OpenRead(path)
	if err != ErrOK {
		_ = std.Puts("long filename write: false\n")
		return 0
	}
	var check [32]byte
	count, readErr := verify.Read(check[:])
	_ = verify.Close()
	if readErr != ErrOK || count != len(content) || string(check[:count]) != string(content) {
		_ = std.Puts("long filename write: false\n")
		return 0
	}
	if err := fs.Unlink(path); err != ErrOK {
		_ = std.Puts("long filename unlink: false\n")
		return 0
	}
	if _, err := fs.Stat(path); err == ErrOK {
		_ = std.Puts("long filename unlink: false\n")
		return 0
	}
	_ = std.Puts("long filename write: true\n")
	_ = std.Puts("long filename unlink: true\n")
	return 0
}
