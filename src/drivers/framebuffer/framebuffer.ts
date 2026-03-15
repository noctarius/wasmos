@external("wasmos", "console_write")
declare function console_write(ptr: i32, len: i32): i32;
@external("wasmos", "framebuffer_info")
declare function framebuffer_info(ptr: i32, len: i32): i32;
@external("wasmos", "framebuffer_pixel")
declare function framebuffer_pixel(x: i32, y: i32, color: i32): i32;

function writeString(text: string): void {
  if (text.length == 0) {
    return;
  }
  let bytes = Uint8Array.wrap(String.UTF8.encode(text, false));
  console_write(bytes.dataStart as i32, bytes.byteLength as i32);
}

const FRAMEBUFFER_INFO_SIZE = 32;
const framebufferInfoBuffer = new Uint8Array(FRAMEBUFFER_INFO_SIZE);
const framebufferInfoPtr = <i32>framebufferInfoBuffer.dataStart;

class FramebufferDetails {
  base: u64;
  size: u64;
  width: i32;
  height: i32;
  stride: i32;
}

function probeFramebuffer(): FramebufferDetails | null {
  if (framebuffer_info(framebufferInfoPtr, FRAMEBUFFER_INFO_SIZE) != 0) {
    return null;
  }

  let info = new FramebufferDetails();
  info.base = load<u64>(framebufferInfoPtr);
  info.size = load<u64>(framebufferInfoPtr + 8);
  info.width = load<i32>(framebufferInfoPtr + 16);
  info.height = load<i32>(framebufferInfoPtr + 20);
  info.stride = load<i32>(framebufferInfoPtr + 24);

  if (info.base == 0 || info.size == 0 ||
      info.width <= 0 || info.height <= 0 || info.stride <= 0) {
    return null;
  }
  if (info.stride < info.width) {
    // Weird stride value, bail out.
    return null;
  }
  return info;
}

export function initialize(_proc_endpoint: i32, _module_count: i32, _arg2: i32, _arg3: i32): i32 {
  let details = probeFramebuffer();
  if (!details) {
    writeString("[framebuffer] not present\n");
    return 0;
  }
  writeString("[framebuffer] painting screen\n");
  for (let y = 0; y < details.height; ++y) {
    for (let x = 0; x < details.width; ++x) {
      let red = ((x * 255) / details.width) & 0xff;
      let green = ((y * 255) / details.height) & 0xff;
      let blue = (((x + y) * 128) / (details.width + details.height)) & 0xff;
      let color = (red << 16) | (green << 8) | blue;
      framebuffer_pixel(x, y, color);
    }
  }
  writeString("[framebuffer] done\n");
  return 0;
}
