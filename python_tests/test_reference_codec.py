from __future__ import annotations

import importlib.util
import gzip
import sys
import tempfile
import unittest
from pathlib import Path


def load_module(name: str, relative_path: str):
    path = Path(__file__).resolve().parents[1] / relative_path
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


reference_codec = load_module("devzip_reference", "native/engine/reference/devzip_reference.py")


class ReferenceCodecTests(unittest.TestCase):
    def test_round_trip_archive_tree(self) -> None:
        with tempfile.TemporaryDirectory() as temp_directory:
            root = Path(temp_directory)
            source = root / "source"
            extracted = root / "extracted"
            archive = root / "sample.dvz"

            source.mkdir()
            (source / "alpha.txt").write_text("CompressionBackend " * 32, encoding="utf-8")
            (source / "nested").mkdir()
            (source / "nested" / "payload.bin").write_bytes(bytes(range(64)) * 8)

            reference_codec.archive_tree(source, archive)
            reference_codec.verify_archive(archive)
            reference_codec.extract_archive(archive, extracted)

            self.assertEqual(
                (source / "alpha.txt").read_text(encoding="utf-8"),
                (extracted / "alpha.txt").read_text(encoding="utf-8"),
            )
            self.assertEqual(
                (source / "nested" / "payload.bin").read_bytes(),
                (extracted / "nested" / "payload.bin").read_bytes(),
            )

    def test_round_trip_single_file_source(self) -> None:
        with tempfile.TemporaryDirectory() as temp_directory:
            root = Path(temp_directory)
            source = root / "single.txt"
            archive = root / "single.dvzr"
            extracted = root / "extracted"

            source.write_text("alpha beta gamma " * 8192, encoding="utf-8")

            reference_codec.archive_tree(source, archive)
            reference_codec.verify_archive(archive)
            reference_codec.extract_archive(archive, extracted)

            self.assertEqual(
                source.read_text(encoding="utf-8"),
                (extracted / "single.txt").read_text(encoding="utf-8"),
            )

    def test_round_trip_gzip_normalized_payload(self) -> None:
        with tempfile.TemporaryDirectory() as temp_directory:
            root = Path(temp_directory)
            source = root / "source"
            extracted = root / "extracted"
            archive = root / "sample.dvzr"

            source.mkdir()
            payload = gzip.compress(b"hello gzip" * 64, mtime=12345)
            (source / "payload.gz").write_bytes(payload)

            reference_codec.archive_tree(source, archive)
            reference_codec.verify_archive(archive)
            reference_codec.extract_archive(archive, extracted)

            self.assertEqual(
                payload,
                (extracted / "payload.gz").read_bytes(),
            )

    def test_safe_destination_path_rejects_parent_escape(self) -> None:
        destination = Path(tempfile.gettempdir()) / "devzip-safe-destination"
        with self.assertRaises(ValueError):
            reference_codec.safe_destination_path(destination, "../outside.txt")

    def test_read_archive_rejects_bad_magic(self) -> None:
        with tempfile.TemporaryDirectory() as temp_directory:
            archive = Path(temp_directory) / "bad.dvz"
            archive.write_bytes(b"BAD!")

            with self.assertRaises(ValueError):
                reference_codec.read_archive(archive)


if __name__ == "__main__":
    unittest.main()
