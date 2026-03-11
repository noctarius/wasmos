#!/usr/bin/env python3
import argparse
import struct
import sys

WASM_MAGIC = b"\0asm"
WASM_VERSION = 1
WASMOS_MAGIC = b"WASMOSAP"


class ReadError(Exception):
    pass


def read_bytes(data, off, size):
    if off + size > len(data):
        raise ReadError("unexpected end of file")
    return data[off:off + size], off + size


def read_u32_le(data, off):
    raw, off = read_bytes(data, off, 4)
    return struct.unpack("<I", raw)[0], off


def read_u16_le(data, off):
    raw, off = read_bytes(data, off, 2)
    return struct.unpack("<H", raw)[0], off


def read_varuint32(data, off):
    result = 0
    shift = 0
    while True:
        if off >= len(data):
            raise ReadError("unexpected end of file")
        b = data[off]
        off += 1
        result |= (b & 0x7F) << shift
        if (b & 0x80) == 0:
            break
        shift += 7
        if shift > 35:
            raise ReadError("varuint32 too large")
    return result, off


def read_name(data, off):
    length, off = read_varuint32(data, off)
    raw, off = read_bytes(data, off, length)
    try:
        return raw.decode("utf-8"), off
    except UnicodeDecodeError:
        return raw.decode("latin1", errors="replace"), off


def parse_wasmos_app(data):
    if len(data) < 8:
        raise ReadError("file too small for WASMOS header")
    if data[:8] != WASMOS_MAGIC:
        return None
    off = 8
    version, off = read_u16_le(data, off)
    header_size, off = read_u16_le(data, off)
    flags, off = read_u32_le(data, off)
    name_len, off = read_u32_le(data, off)
    entry_len, off = read_u32_le(data, off)
    wasm_size, off = read_u32_le(data, off)
    req_ep_count, off = read_u32_le(data, off)
    cap_count, off = read_u32_le(data, off)
    mem_hint_count, off = read_u32_le(data, off)
    _reserved, off = read_u32_le(data, off)
    header_end = header_size
    name_raw, off = read_bytes(data, off, name_len)
    entry_raw, off = read_bytes(data, off, entry_len)
    for _ in range(req_ep_count):
        _name_len, off = read_u32_le(data, off)
        _rights, off = read_u32_le(data, off)
        _, off = read_bytes(data, off, _name_len)
    for _ in range(cap_count):
        _name_len, off = read_u32_le(data, off)
        _flags, off = read_u32_le(data, off)
        _, off = read_bytes(data, off, _name_len)
    for _ in range(mem_hint_count):
        _, off = read_u32_le(data, off)
        _, off = read_u32_le(data, off)
        _, off = read_u32_le(data, off)
    if header_end < off:
        header_end = off
    wasm_off = header_end
    if wasm_off + wasm_size > len(data):
        raise ReadError("wasm payload exceeds file size")
    return {
        "version": version,
        "flags": flags,
        "name": name_raw.decode("utf-8", errors="replace"),
        "entry": entry_raw.decode("utf-8", errors="replace"),
        "wasm_offset": wasm_off,
        "wasm_size": wasm_size,
    }


def parse_wasm(data):
    if len(data) < 8:
        raise ReadError("file too small for wasm header")
    if data[:4] != WASM_MAGIC:
        raise ReadError("missing wasm magic")
    version = struct.unpack("<I", data[4:8])[0]
    if version != WASM_VERSION:
        raise ReadError(f"unexpected wasm version {version}")

    off = 8
    imports = []
    exports = []
    type_count = None
    func_count = None
    code_count = None

    while off < len(data):
        section_id = data[off]
        off += 1
        size, off = read_varuint32(data, off)
        payload, off = read_bytes(data, off, size)
        p = 0
        if section_id == 1:
            count, p = read_varuint32(payload, p)
            type_count = count
        elif section_id == 2:
            count, p = read_varuint32(payload, p)
            for _ in range(count):
                mod, p = read_name(payload, p)
                name, p = read_name(payload, p)
                kind = payload[p]
                p += 1
                if kind == 0:
                    _, p = read_varuint32(payload, p)
                    kind_name = "func"
                elif kind == 1:
                    _, p = read_varuint32(payload, p)
                    _, p = read_varuint32(payload, p)
                    kind_name = "table"
                elif kind == 2:
                    _, p = read_varuint32(payload, p)
                    limits_flag, p = read_varuint32(payload, p)
                    if limits_flag & 1:
                        _, p = read_varuint32(payload, p)
                    kind_name = "memory"
                elif kind == 3:
                    _, p = read_varuint32(payload, p)
                    _, p = read_varuint32(payload, p)
                    kind_name = "global"
                else:
                    kind_name = f"unknown({kind})"
                imports.append((mod, name, kind_name))
        elif section_id == 3:
            count, p = read_varuint32(payload, p)
            func_count = count
        elif section_id == 7:
            count, p = read_varuint32(payload, p)
            for _ in range(count):
                name, p = read_name(payload, p)
                kind = payload[p]
                p += 1
                _, p = read_varuint32(payload, p)
                if kind == 0:
                    kind_name = "func"
                elif kind == 1:
                    kind_name = "table"
                elif kind == 2:
                    kind_name = "memory"
                elif kind == 3:
                    kind_name = "global"
                else:
                    kind_name = f"unknown({kind})"
                exports.append((name, kind_name))
        elif section_id == 10:
            count, p = read_varuint32(payload, p)
            code_count = count

    return {
        "type_count": type_count,
        "func_count": func_count,
        "code_count": code_count,
        "imports": imports,
        "exports": exports,
    }


def main():
    parser = argparse.ArgumentParser(description="Inspect wasm or .wasmosapp files.")
    parser.add_argument("path", help="Path to .wasm or .wasmosapp")
    parser.add_argument("--raw", action="store_true", help="Print raw import/export lists without headers")
    args = parser.parse_args()

    with open(args.path, "rb") as f:
        data = f.read()

    app_info = None
    if data.startswith(WASMOS_MAGIC):
        app_info = parse_wasmos_app(data)
        wasm_data = data[app_info["wasm_offset"]:app_info["wasm_offset"] + app_info["wasm_size"]]
    else:
        wasm_data = data

    info = parse_wasm(wasm_data)

    if not args.raw:
        if app_info:
            print("WASMOS-APP")
            print(f"name: {app_info['name']}")
            print(f"entry: {app_info['entry']}")
            print(f"flags: 0x{app_info['flags']:08X}")
            print(f"wasm_size: {app_info['wasm_size']}")
            print("")
        print("WASM")
        if info["type_count"] is not None:
            print(f"types: {info['type_count']}")
        if info["func_count"] is not None:
            print(f"functions: {info['func_count']}")
        if info["code_count"] is not None:
            print(f"code: {info['code_count']}")
        print("")
        print("imports:")
    for mod, name, kind in info["imports"]:
        print(f"{mod}.{name} ({kind})")
    if not args.raw:
        print("")
        print("exports:")
    for name, kind in info["exports"]:
        print(f"{name} ({kind})")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
