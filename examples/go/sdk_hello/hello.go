// sdk_hello (Go) - the Go driver's own smoke app.
//
// Plain Go against the wasmos binding, built by the staged SDK
// (cmake/WasmosSdk.cmake) with no flags but -o and no linker.metadata of its own.
//
// What it exercises is the most involved staging of any driver. TinyGo is
// configured by a target FILE rather than flags, and that file cannot be shipped
// ready-made: its extra-files (the C shims a Go guest links) are resolved relative
// to TINYGOROOT, wherever TinyGo happens to be installed, so wasmos-tinygo
// generates the target per invocation after asking tinygo where its root is.
// TinyGo also builds a package, not a file, so this and the two Go runtime files
// are staged into one directory.
//
// The entry is Main with a capital M -- the binding's wasmos_main export calls it.
//
// Deliberately minimal. The Go binding's breadth is covered by examples/go/hello;
// what is under test here is the driver.
package main

func Main(args []string) int32 {
	_ = std.Puts("Hello WASMOS from Go via the SDK!\n")
	return 0
}
