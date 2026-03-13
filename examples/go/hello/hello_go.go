package main

func main() {
	wasmos_entry(0, 0, 0, 0)
}

//export wasmos_entry
func wasmos_entry(_arg0, _arg1, _arg2, _arg3 int32) {
	putsn("Hello from Go on WASMOS!\n")
	putsn("This is a tiny WASMOS-APP written in Go.\n")
	putsn("Entry: main\n")
}
