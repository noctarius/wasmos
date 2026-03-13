@external("wasmos", "console_write")
declare function console_write(ptr: i32, len: i32): i32;

export function putsn(msg: string): void {
  const buf = new Uint8Array(1);
  for (let i = 0; i < msg.length; i++) {
    buf[0] = msg.charCodeAt(i) as u8;
    console_write(buf.dataStart as i32, 1);
  }
}
