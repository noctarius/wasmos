package main

import "unsafe"

//go:wasmimport wasmos console_write
func consoleWrite(ptr uint32, len uint32) int32

func putsn(s string) int32 {
	if len(s) == 0 {
		return 0
	}
	ptr := unsafe.StringData(s)
	return consoleWrite(uint32(uintptr(unsafe.Pointer(ptr))), uint32(len(s)))
}
