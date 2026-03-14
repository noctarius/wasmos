#!/usr/bin/env python3

import argparse
import pathlib
import struct
import tomllib


INITFS_MAGIC = b"WMINITFS"
INITFS_VERSION = 1
INITFS_HEADER_STRUCT = struct.Struct("<8sHHIIII")
INITFS_ENTRY_STRUCT = struct.Struct("<IIII96s")

CONFIG_MAGIC = b"WCFG0001"
CONFIG_HEADER_STRUCT = struct.Struct("<8sIIII")
CONFIG_OFFSET_STRUCT = struct.Struct("<I")
SYSINIT_SPAWN_NAME_MAX = 16

ENTRY_KIND = {
    "wasmos_app": 1,
    "config": 2,
    "data": 3,
}

ENTRY_FLAG = {
    "bootstrap": 1 << 0,
}


def encode_c_string(value: str, width: int) -> bytes:
    raw = value.encode("ascii")
    if len(raw) >= width:
        raise ValueError(f"string too long for fixed field: {value}")
    return raw + (b"\0" * (width - len(raw)))


def build_boot_config(manifest: dict) -> bytes:
    boot_names = manifest.get("boot", {}).get("bootstrap_modules", [])
    sysinit_names = manifest.get("sysinit", {}).get("spawn", [])
    strings = bytearray()
    boot_offsets: list[int] = []
    sysinit_offsets: list[int] = []
    sysinit_seen: set[str] = set()

    def add_name(name: str) -> int:
        if not isinstance(name, str) or not name:
            raise ValueError("config names must be non-empty strings")
        offset = len(strings)
        strings.extend(name.encode("ascii"))
        strings.append(0)
        return offset

    for name in boot_names:
        boot_offsets.append(add_name(name))
    if not isinstance(sysinit_names, list) or not sysinit_names:
        raise ValueError("sysinit.spawn must list at least one process")
    for name in sysinit_names:
        if name in sysinit_seen:
            raise ValueError("sysinit.spawn names must be unique")
        if len(name.encode("ascii")) > SYSINIT_SPAWN_NAME_MAX:
            raise ValueError("sysinit.spawn names must fit the 16-byte PM spawn ABI")
        sysinit_seen.add(name)
        sysinit_offsets.append(add_name(name))

    out = bytearray()
    out.extend(
        CONFIG_HEADER_STRUCT.pack(
            CONFIG_MAGIC,
            1,
            len(boot_offsets),
            len(sysinit_offsets),
            len(strings),
        )
    )
    for offset in boot_offsets:
        out.extend(CONFIG_OFFSET_STRUCT.pack(offset))
    for offset in sysinit_offsets:
        out.extend(CONFIG_OFFSET_STRUCT.pack(offset))
    out.extend(strings)
    return bytes(out)


def build_initfs(manifest: dict, build_dir: pathlib.Path) -> tuple[bytes, bytes]:
    config_blob = build_boot_config(manifest)
    entry_payloads: list[tuple[dict, bytes]] = []

    for entry in manifest.get("entries", []):
        kind = entry.get("kind")
        if kind not in ENTRY_KIND:
            raise ValueError(f"unsupported initfs entry kind: {kind}")
        if "generated" in entry:
            generated = entry["generated"]
            if generated != "boot_config":
                raise ValueError(f"unsupported generated payload: {generated}")
            payload = config_blob
        else:
            source = entry.get("source")
            if not isinstance(source, str) or not source:
                raise ValueError("initfs source must be a non-empty string")
            payload = (build_dir / source).read_bytes()
        entry_payloads.append((entry, payload))

    header_size = INITFS_HEADER_STRUCT.size
    entry_size = INITFS_ENTRY_STRUCT.size
    data_offset = header_size + len(entry_payloads) * entry_size
    cursor = data_offset
    initfs = bytearray(b"\0" * data_offset)
    packed_entries: list[bytes] = []

    for entry, payload in entry_payloads:
        flags = 0
        for flag_name in entry.get("flags", []):
            if flag_name not in ENTRY_FLAG:
                raise ValueError(f"unsupported initfs flag: {flag_name}")
            flags |= ENTRY_FLAG[flag_name]
        path = entry.get("path")
        if not isinstance(path, str) or not path:
            raise ValueError("initfs path must be a non-empty string")
        packed_entries.append(
            INITFS_ENTRY_STRUCT.pack(
                ENTRY_KIND[entry["kind"]],
                flags,
                cursor,
                len(payload),
                encode_c_string(path, 96),
            )
        )
        initfs.extend(payload)
        cursor += len(payload)

    header = INITFS_HEADER_STRUCT.pack(
        INITFS_MAGIC,
        INITFS_VERSION,
        header_size,
        len(packed_entries),
        entry_size,
        len(initfs),
        0,
    )
    initfs[0:header_size] = header
    offset = header_size
    for packed_entry in packed_entries:
        initfs[offset:offset + entry_size] = packed_entry
        offset += entry_size

    return bytes(initfs), config_blob


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--config-output", required=True)
    args = parser.parse_args()

    manifest_path = pathlib.Path(args.manifest)
    build_dir = pathlib.Path(args.build_dir)
    output_path = pathlib.Path(args.output)
    config_output_path = pathlib.Path(args.config_output)

    manifest = tomllib.loads(manifest_path.read_text(encoding="utf-8"))
    initfs_blob, config_blob = build_initfs(manifest, build_dir)
    output_path.write_bytes(initfs_blob)
    config_output_path.write_bytes(config_blob)


if __name__ == "__main__":
    main()
