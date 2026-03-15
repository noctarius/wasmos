@external("wasmos", "console_write")
declare function console_write(ptr: i32, len: i32): i32;
@external("wasmos", "framebuffer_info")
declare function framebuffer_info(ptr: i32, len: i32): i32;
@external("wasmos", "framebuffer_map")
declare function framebuffer_map(ptr: i32, size: i32): i32;
@external("wasmos", "framebuffer_pixel")
declare function framebuffer_pixel(x: i32, y: i32, color: i32): i32;

const FRAMEBUFFER_INFO_SIZE = 32;
const FRAMEBUFFER_PAGE_SIZE = 0x1000;
const framebufferInfoBuffer = new Uint8Array(FRAMEBUFFER_INFO_SIZE);
const framebufferInfoPtr = <i32>framebufferInfoBuffer.dataStart;
let framebuffer_storage: Uint8Array | null = null;

class FramebufferDetails {
  base: u64;
  size: u64;
  width: i32;
  height: i32;
  stride: i32;
}

function alignUp(value: i32, align: i32): i32 {
  let mask = align - 1;
  return (value + mask) & ~mask;
}

function writeString(text: string): void {
  if (text.length == 0) {
    return;
  }
  let bytes = Uint8Array.wrap(String.UTF8.encode(text, false));
  console_write(bytes.dataStart as i32, bytes.byteLength as i32);
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
    return null;
  }
  return info;
}

function paintGradient(ptr: i32, info: FramebufferDetails): void {
  let stride = info.stride;
  for (let y = 0; y < info.height; ++y) {
    for (let x = 0; x < info.width; ++x) {
      let red = ((x * 255) / info.width) & 0xff;
      let green = ((y * 255) / info.height) & 0xff;
      let blue = (((x + y) * 128) / (info.width + info.height)) & 0xff;
      let color = (red << 16) | (green << 8) | blue;
      let offset = ptr + ((y * stride + x) << 2);
      store<u32>(offset, color);
    }
  }
}

function paintGradientFallback(info: FramebufferDetails): void {
  for (let y = 0; y < info.height; ++y) {
    for (let x = 0; x < info.width; ++x) {
      let red = ((x * 255) / info.width) & 0xff;
      let green = ((y * 255) / info.height) & 0xff;
      let blue = (((x + y) * 128) / (info.width + info.height)) & 0xff;
      let color = (red << 16) | (green << 8) | blue;
      framebuffer_pixel(x, y, color);
    }
  }
}

export function initialize(_proc_endpoint: i32, _module_count: i32, _arg2: i32, _arg3: i32): i32 {
  let details = probeFramebuffer();
  if (!details) {
    writeString("[framebuffer] not present\n");
    return 0;
  }

  let rawSize = <i32>details.size;
  let mapSize = ((rawSize + FRAMEBUFFER_PAGE_SIZE - 1) / FRAMEBUFFER_PAGE_SIZE) * FRAMEBUFFER_PAGE_SIZE;
  framebuffer_storage = new Uint8Array(rawSize + FRAMEBUFFER_PAGE_SIZE);
  if (!framebuffer_storage) {
    writeString("[framebuffer] allocation failed\n");
    return 0;
  }
  let storage = framebuffer_storage!;
  let base = storage.dataStart as i32;
  let aligned = alignUp(base, FRAMEBUFFER_PAGE_SIZE);
  let available = base + storage.byteLength;
  if (aligned + mapSize > available) {
    writeString("[framebuffer] buffer alignment failed\n");
    return 0;
  }

  writeString("[framebuffer] mapping framebuffer\n");
  if (framebuffer_map(aligned, mapSize) == 0) {
    writeString("[framebuffer] painting mapped buffer\n");
    paintGradient(aligned, details);
  } else {
    writeString("[framebuffer] map failed, falling back\n");
    paintGradientFallback(details);
  }
  writeString("[framebuffer] done\n");
  return 0;
}
