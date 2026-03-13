package main

func main() {
	wasmos_entry(0, 0, 0, 0)
}

//export wasmos_entry
func wasmos_entry(_arg0, _arg1, _arg2, _arg3 int32) {
	_ = std.Puts("Hello from Go on WASMOS!\n")
	_ = std.Puts("This is a tiny WASMOS-APP written in Go.\n")
	_ = std.Puts("Entry: main\n")

	file, err := fs.OpenRead("/startup.nsh")
	if err != ErrOK {
		_ = std.Puts("startup.nsh readable: false\n")
		return
	}

	var buf [1]byte
	n, readErr := file.Read(buf[:])
	_ = file.Close()
	if readErr != ErrOK || n <= 0 {
		_ = std.Puts("startup.nsh readable: false\n")
		return
	}

	_ = std.Puts("startup.nsh readable: true\n")
}
