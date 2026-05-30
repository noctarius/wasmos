#!/usr/bin/env python3

import argparse
import pathlib
import struct
import tomllib


INITFS_MAGIC = b"WMINITFS"
INITFS_VERSION = 1
INITFS_HEADER_STRUCT = struct.Struct("<8sHHIIII")
INITFS_ENTRY_STRUCT = struct.Struct("<IIII96s")

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
    return raw + (b"\x00" * (width - len(raw)))


def build_initfs(manifest: dict, build_dir: pathlib.Path) -> bytes:
    entry_payloads: list[tuple[dict, bytes]] = []

    for entry in manifest.get("entries", []):
        kind = entry.get("kind")
        if kind not in ENTRY_KIND:
            raise ValueError(f"unsupported initfs entry kind: {kind}")
        source = entry.get("source")
        if not isinstance(source, str) or not source:
            raise ValueError("initfs source must be a non-empty string")
        payload = (build_dir / source).read_bytes()
        entry_payloads.append((entry, payload))

    header_size = INITFS_HEADER_STRUCT.size
    entry_size = INITFS_ENTRY_STRUCT.size
    data_offset = header_size + len(entry_payloads) * entry_size
    cursor = data_offset
    initfs = bytearray(b"\x00" * data_offset)
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

    return bytes(initfs)


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
    initfs_blob = build_initfs(manifest, build_dir)
    output_path.write_bytes(initfs_blob)
    config_output_path.write_bytes(b"")


if __name__ == "__main__":
    main()
