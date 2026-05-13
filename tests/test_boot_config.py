import os
import pathlib
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

import make_initfs
from qemu_test_framework import QemuSession, default_config


def _entry_path(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("ascii")


def _corrupt_sysinit_offset(initfs_blob: bytes) -> bytes:
    image = bytearray(initfs_blob)
    magic, version, header_size, entry_count, entry_size, image_size, _ = make_initfs.INITFS_HEADER_STRUCT.unpack_from(image, 0)
    if magic != make_initfs.INITFS_MAGIC or version != make_initfs.INITFS_VERSION or image_size != len(image):
        raise ValueError("unexpected initfs image")

    for i in range(entry_count):
        offset = header_size + i * entry_size
        kind, _flags, payload_offset, payload_size, path_raw = make_initfs.INITFS_ENTRY_STRUCT.unpack_from(image, offset)
        if kind != make_initfs.ENTRY_KIND["config"]:
            continue
        if _entry_path(path_raw) != "config/bootcfg.bin":
            continue

        payload = bytearray(image[payload_offset:payload_offset + payload_size])
        magic, _cfg_version, boot_count, sysinit_count, string_table_size = make_initfs.CONFIG_HEADER_STRUCT.unpack_from(payload, 0)
        if magic != make_initfs.CONFIG_MAGIC or sysinit_count == 0:
            raise ValueError("unexpected boot config payload")

        first_sysinit_offset = make_initfs.CONFIG_HEADER_STRUCT.size + boot_count * make_initfs.CONFIG_OFFSET_STRUCT.size
        make_initfs.CONFIG_OFFSET_STRUCT.pack_into(payload, first_sysinit_offset, string_table_size)
        image[payload_offset:payload_offset + payload_size] = payload
        return bytes(image)

    raise ValueError("boot config payload not found")


class BootConfigManifestTests(unittest.TestCase):
    def test_build_boot_config_rejects_empty_sysinit_spawn(self):
        manifest = {
            "boot": {"bootstrap_modules": ["device-manager"]},
            "sysinit": {"spawn": []},
        }
        with self.assertRaisesRegex(ValueError, "sysinit.spawn must list at least one process"):
            make_initfs.build_boot_config(manifest)

    def test_build_boot_config_rejects_duplicate_sysinit_spawn(self):
        manifest = {
            "boot": {"bootstrap_modules": ["device-manager"]},
            "sysinit": {"spawn": ["cli", "cli"]},
        }
        with self.assertRaisesRegex(ValueError, "sysinit.spawn names must be unique"):
            make_initfs.build_boot_config(manifest)

    def test_build_boot_config_rejects_long_sysinit_name(self):
        manifest = {
            "boot": {"bootstrap_modules": ["device-manager"]},
            "sysinit": {"spawn": ["abcdefghijklmnopq"]},
        }
        with self.assertRaisesRegex(ValueError, "16-byte PM spawn ABI"):
            make_initfs.build_boot_config(manifest)


class MalformedBootConfigQemuTests(unittest.TestCase):
    session: QemuSession | None = None
    esp_initfs_path: pathlib.Path | None = None
    original_initfs: bytes | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        cls.esp_initfs_path = pathlib.Path(cfg.esp_dir) / "initfs.img"
        cls.original_initfs = cls.esp_initfs_path.read_bytes()
        try:
            cls.esp_initfs_path.write_bytes(_corrupt_sysinit_offset(cls.original_initfs))
            cls.session = QemuSession(cfg, timeout_s=60, echo=True, force_stop_on_timeout=False)
            cls.session.start()
        except Exception:
            if cls.esp_initfs_path is not None and cls.original_initfs is not None:
                cls.esp_initfs_path.write_bytes(cls.original_initfs)
            raise

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None
        if cls.esp_initfs_path is not None and cls.original_initfs is not None:
            cls.esp_initfs_path.write_bytes(cls.original_initfs)

    def test_sysinit_rejects_corrupted_boot_config(self) -> None:
        assert self.session is not None
        ok = self.session.expect(b"[sysinit] invalid boot config", timeout_s=30)
        self.assertTrue(ok, f"sysinit failure marker not observed\n--- tail ---\n{self.session.tail()}\n")
        ok = self.session.expect(b"wamos> ", timeout_s=5)
        self.assertFalse(ok, "CLI prompt should not appear after malformed boot config")


if __name__ == "__main__":
    unittest.main()
