#[link(wasm_import_module = "wasmos")]
extern "C" {
    fn console_write(ptr: i32, len: i32) -> i32;
}

#[inline]
pub fn putsn(bytes: &[u8]) -> i32 {
    if bytes.is_empty() {
        return 0;
    }
    unsafe { console_write(bytes.as_ptr() as i32, bytes.len() as i32) }
}
