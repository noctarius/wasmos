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
    entry_arg_binding_count = 0
    if version >= 2:
        entry_arg_binding_count, off = read_u32_le(data, off)
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
    for _ in range(entry_arg_binding_count):
        _name_len, off = read_u32_le(data, off)
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
    func_imports = []
    exports = []
    type_count = None
    func_count = None
    code_count = None
    code_bodies = []
    data_count = None
    data_total = 0
    table_count = None
    elem_count = None
    elem_segments = []

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
                    func_imports.append((mod, name))
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
                index, p = read_varuint32(payload, p)
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
                exports.append((name, kind_name, index))
        elif section_id == 10:
            count, p = read_varuint32(payload, p)
            code_count = count
            code_bodies = []
            for _ in range(count):
                body_size, p = read_varuint32(payload, p)
                body, p = read_bytes(payload, p, body_size)
                code_bodies.append(body)
        elif section_id == 4:
            count, p = read_varuint32(payload, p)
            table_count = count
            for _ in range(count):
                _, p = read_bytes(payload, p, 1)
                flags, p = read_varuint32(payload, p)
                _, p = read_varuint32(payload, p)
                if flags & 1:
                    _, p = read_varuint32(payload, p)
        elif section_id == 9:
            count, p = read_varuint32(payload, p)
            elem_count = count
            for _ in range(count):
                flags, p = read_varuint32(payload, p)
                if flags in (0, 2):
                    if flags == 2:
                        _, p = read_varuint32(payload, p)
                    while p < len(payload) and payload[p] != 0x0b:
                        p += 1
                    if p < len(payload):
                        p += 1
                    elem_count_entries, p = read_varuint32(payload, p)
                    indices = []
                    for _ in range(elem_count_entries):
                        idx, p = read_varuint32(payload, p)
                        indices.append(idx)
                    elem_segments.append(indices)
                else:
                    elem_count_entries, p = read_varuint32(payload, p)
                    for _ in range(elem_count_entries):
                        _, p = read_varuint32(payload, p)
        elif section_id == 11:
            count, p = read_varuint32(payload, p)
            data_count = count
            total = 0
            for _ in range(count):
                flags, p = read_varuint32(payload, p)
                if flags == 0:
                    while p < len(payload) and payload[p] != 0x0b:
                        p += 1
                    if p < len(payload):
                        p += 1
                elif flags == 1:
                    pass
                elif flags == 2:
                    _, p = read_varuint32(payload, p)
                    while p < len(payload) and payload[p] != 0x0b:
                        p += 1
                    if p < len(payload):
                        p += 1
                elif flags == 3:
                    _, p = read_varuint32(payload, p)
                data_size, p = read_varuint32(payload, p)
                total += data_size
                _, p = read_bytes(payload, p, data_size)
            data_total = total

    return {
        "type_count": type_count,
        "func_count": func_count,
        "code_count": code_count,
        "imports": imports,
        "func_imports": func_imports,
        "exports": exports,
        "code_bodies": code_bodies,
        "data_count": data_count,
        "data_total": data_total,
        "table_count": table_count,
        "elem_count": elem_count,
        "elem_segments": elem_segments,
    }


def parse_locals(body, off):
    count, off = read_varuint32(body, off)
    for _ in range(count):
        _, off = read_varuint32(body, off)
        _, off = read_bytes(body, off, 1)
    return off


def skip_memarg(body, off):
    _, off = read_varuint32(body, off)
    _, off = read_varuint32(body, off)
    return off


def scan_calls_in_body(body):
    calls = []
    off = parse_locals(body, 0)
    while off < len(body):
        op = body[off]
        off += 1
        if op == 0x0b:
            break
        if op in (0x02, 0x03, 0x04):
            if off >= len(body):
                break
            if body[off] == 0x40:
                off += 1
            else:
                _, off = read_varuint32(body, off)
        elif op == 0x05:
            continue
        elif op in (0x0c, 0x0d):
            _, off = read_varuint32(body, off)
        elif op == 0x0e:
            cnt, off = read_varuint32(body, off)
            for _ in range(cnt):
                _, off = read_varuint32(body, off)
            _, off = read_varuint32(body, off)
        elif op == 0x0f:
            continue
        elif op == 0x10:
            idx, off = read_varuint32(body, off)
            calls.append(idx)
        elif op == 0x11:
            _, off = read_varuint32(body, off)
            if off < len(body):
                off += 1
            calls.append(("indirect", None))
        elif op in (0x1a, 0x1b):
            continue
        elif op in (0x20, 0x21, 0x22, 0x23, 0x24):
            _, off = read_varuint32(body, off)
        elif op in (0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
                    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
                    0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e):
            off = skip_memarg(body, off)
        elif op in (0x3f, 0x40):
            if off < len(body):
                off += 1
        elif op == 0x41:
            _, off = read_varuint32(body, off)
        elif op == 0x42:
            _, off = read_varuint32(body, off)
        elif op == 0x43:
            off += 4
        elif op == 0x44:
            off += 8
        elif op == 0xfc or op == 0xfd:
            _, off = read_varuint32(body, off)
        else:
            continue
    return calls


def main():
    parser = argparse.ArgumentParser(description="Inspect wasm or .wap files.")
    parser.add_argument("path", help="Path to .wasm or .wap")
    parser.add_argument("--raw", action="store_true", help="Print raw import/export lists without headers")
    parser.add_argument("--calls", action="store_true", help="Scan function bodies for call opcodes")
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
        if info["data_count"] is not None:
            print(f"data segments: {info['data_count']} bytes={info['data_total']}")
        if info["table_count"] is not None:
            print(f"tables: {info['table_count']}")
        if info["elem_count"] is not None:
            print(f"elements: {info['elem_count']}")
        if info.get("elem_segments"):
            for i, seg in enumerate(info["elem_segments"]):
                preview = ", ".join(str(x) for x in seg[:8])
                if len(seg) > 8:
                    preview += ", ..."
                print(f"elem[{i}] indices: {preview}")
        print("")
        print("imports:")
    for mod, name, kind in info["imports"]:
        print(f"{mod}.{name} ({kind})")
    if not args.raw:
        print("")
        print("exports:")
    for name, kind, index in info["exports"]:
        print(f"{name} ({kind}) idx={index}")

    if args.calls and info["code_bodies"]:
        print("")
        print("calls:")
        import_count = len(info["func_imports"])
        for i, body in enumerate(info["code_bodies"]):
            calls = scan_calls_in_body(body)
            if not calls:
                print(f"func[{i}] calls: (none)")
                continue
            parts = []
            for idx in calls:
                if isinstance(idx, tuple) and idx[0] == "indirect":
                    parts.append("call_indirect")
                    continue
                if idx < import_count:
                    mod, name = info["func_imports"][idx]
                    parts.append(f"{idx}:{mod}.{name}")
                else:
                    parts.append(f"{idx}:func")
            print(f"func[{i}] calls: " + ", ".join(parts))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
