package main

func Main(args []string) int32 {
	_ = args
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
	return 0
}
