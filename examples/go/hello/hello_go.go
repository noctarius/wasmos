package main

import "unsafe"

const wasmosStepYielded = 0

//go:wasmimport wasmos console_write
func console_write(ptr uint32, len uint32) int32

func writeLine(line []byte) {
	if len(line) == 0 {
		return
	}
	console_write(uint32(uintptr(unsafe.Pointer(&line[0]))), uint32(len(line)))
}

var printed bool

//export hello_go_step
func hello_go_step(_type, _arg0, _arg1, _arg2, _arg3 int32) int32 {
	if !printed {
		printed = true
		writeLine([]byte("Hello from Go on WASMOS!\n"))
		writeLine([]byte("This is a tiny WASMOS-APP written in Go.\n"))
		writeLine([]byte("Entry: hello_go_step\n"))
	}
	return wasmosStepYielded
}

func main() {}
