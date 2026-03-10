package main

import "unsafe"

//go:wasmimport wasmos console_write
func console_write(ptr uint32, len uint32) int32

func writeLine(line []byte) {
	if len(line) == 0 {
		return
	}
	console_write(uint32(uintptr(unsafe.Pointer(&line[0]))), uint32(len(line)))
}

func main() {
	wasmos_entry(0, 0, 0, 0)
}

//export wasmos_entry
func wasmos_entry(_arg0, _arg1, _arg2, _arg3 int32) {
	writeLine([]byte("Hello from Go on WASMOS!\n"))
	writeLine([]byte("This is a tiny WASMOS-APP written in Go.\n"))
	writeLine([]byte("Entry: main\n"))
}
