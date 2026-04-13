from __future__ import annotations

import argparse
import hashlib
import json
import lzma
import os
import stat
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator


MAGIC = b"DVR1"
HEADER_STRUCT = struct.Struct("<4sBQ")
MAX_MANIFEST_BYTES = 256 * 1024 * 1024
MAX_CHUNK_BYTES = 256 * 1024 * 1024


@dataclass(frozen=True)
class ChunkSpec:
    chunk_id: str
    offset: int
    raw_size: int
    compressed_size: int
    checksum: str
    transforms: list[str]
    recipe: str


def iter_paths(root: Path) -> Iterator[Path]:
    if root.is_file():
        yield root
        return

    yield root
    for path in sorted(root.rglob("*"), key=lambda item: item.as_posix().lower()):
        yield path


def relative_name(root: Path, path: Path) -> str:
    if root.is_file():
        return path.name
    if path == root:
        return "."
    return path.relative_to(root).as_posix()


def safe_destination_path(destination: Path, relative: str) -> Path:
    candidate = Path(relative)
    if candidate.is_absolute() or ".." in candidate.parts:
        raise ValueError(f"Archive entry escapes destination root: {relative}")
    return destination if relative == "." else destination / candidate


def fastcdc_chunks(data: bytes, *, minimum: int = 8 * 1024, average: int = 16 * 1024, maximum: int = 64 * 1024) -> list[bytes]:
    if len(data) <= maximum:
        return [data]

    mask = average - 1
    window_hash = 0
    start = 0
    chunks: list[bytes] = []

    for index, byte in enumerate(data):
        window_hash = ((window_hash << 5) + window_hash + byte) & 0xFFFFFFFF
        chunk_size = index - start + 1
        if chunk_size < minimum:
            continue
        if chunk_size >= maximum or (window_hash & mask) == 0:
            chunks.append(data[start:index + 1])
            start = index + 1
            window_hash = 0

    if start < len(data):
        chunks.append(data[start:])
    return chunks


def checksum_bytes(data: bytes) -> str:
    return hashlib.blake2b(data, digest_size=16).hexdigest()


def compress_chunk(data: bytes) -> bytes:
    return lzma.compress(data, preset=9 | lzma.PRESET_EXTREME, check=lzma.CHECK_CRC64)


def looks_like_text(data: bytes) -> bool:
    if not data:
        return False
    printable = 0
    for value in data:
        if 32 <= value <= 126 or value in (9, 10, 13):
            printable += 1
    return printable * 100 // len(data) >= 85


def apply_stream_normalization(data: bytes) -> tuple[bytes, list[str], str]:
    if len(data) >= 10 and data[:4] == b"\x1f\x8b\x08\x00":
        recipe = "gzip:" + data[4:10].hex()
        normalized = data[:4] + b"\x00\x00\x00\x00\x00\x00" + data[10:]
        return normalized, ["stream_normalization"], recipe
    return data, [], ""


def reverse_stream_normalization(data: bytes, recipe: str) -> bytes:
    if not recipe.startswith("gzip:"):
        return data
    original = bytes.fromhex(recipe[5:])
    return data[:4] + original + data[10:]


def apply_code_dictionary(data: bytes) -> tuple[bytes, list[str], str]:
    if not looks_like_text(data):
        return data, [], ""

    counts: dict[str, int] = {}
    token = []
    for value in data:
        ch = chr(value)
        if ch.isalnum() or ch == "_":
            token.append(ch)
            continue
        if len(token) >= 4:
            counts["".join(token)] = counts.get("".join(token), 0) + 1
        token.clear()
    if len(token) >= 4:
        counts["".join(token)] = counts.get("".join(token), 0) + 1

    candidates = sorted(
        (
            (word, count * (len(word) - 2))
            for word, count in counts.items()
            if count > 1 and count * (len(word) - 2) > len(word)
        ),
        key=lambda item: (-item[1], -len(item[0]), item[0]),
    )[:16]
    dictionary = [word for word, _ in candidates]
    if not dictionary:
        return data, [], ""

    indexes = {word: index + 1 for index, word in enumerate(dictionary)}
    output = bytearray()
    position = 0
    while position < len(data):
        boundary_start = position == 0 or not (chr(data[position - 1]).isalnum() or data[position - 1] == ord("_"))
        match = None
        if boundary_start:
            for word in dictionary:
                if data[position:position + len(word)] != word.encode("utf-8"):
                    continue
                next_index = position + len(word)
                boundary_end = next_index == len(data) or not (
                    chr(data[next_index]).isalnum() or data[next_index] == ord("_")
                )
                if boundary_end and (match is None or len(word) > len(match)):
                    match = word
        if match:
            output.extend((0xFF, indexes[match]))
            position += len(match)
            continue
        value = data[position]
        if value == 0xFF:
            output.extend((0xFF, 0))
        else:
            output.append(value)
        position += 1

    if len(output) >= len(data):
        return data, [], ""

    return bytes(output), ["code_dictionary"], "\n".join(dictionary)


def reverse_code_dictionary(data: bytes, recipe: str) -> bytes:
    if not recipe:
        return data
    dictionary = [line for line in recipe.splitlines() if line]
    if not dictionary:
        return data

    output = bytearray()
    index = 0
    while index < len(data):
        value = data[index]
        if value != 0xFF:
            output.append(value)
            index += 1
            continue
        if index + 1 >= len(data):
            output.append(value)
            break
        code = data[index + 1]
        if code == 0:
            output.append(0xFF)
        elif 1 <= code <= len(dictionary):
            output.extend(dictionary[code - 1].encode("utf-8"))
        else:
            output.extend((0xFF, code))
        index += 2
    return bytes(output)


def transform_chunk(data: bytes) -> tuple[bytes, list[str], str]:
    normalized, transforms, recipe = apply_stream_normalization(data)
    if transforms:
        return normalized, transforms, recipe
    dictionary_encoded, transforms, recipe = apply_code_dictionary(data)
    if transforms:
        return dictionary_encoded, transforms, recipe
    return data, [], ""


def restore_chunk(data: bytes, transforms: list[str], recipe: str) -> bytes:
    if transforms == ["stream_normalization"]:
        return reverse_stream_normalization(data, recipe)
    if transforms == ["code_dictionary"]:
        return reverse_code_dictionary(data, recipe)
    return data


def archive_tree(source: Path, destination: Path) -> None:
    source = source.resolve()
    destination = destination.resolve()

    manifest: dict[str, object] = {
        "format": "dvz-reference",
        "version": 1,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "source_name": source.name,
        "entries": [],
        "chunks": [],
    }

    chunk_table: dict[str, dict[str, object]] = {}
    chunk_specs: list[ChunkSpec] = []

    for path in iter_paths(source):
        relative = relative_name(source, path)
        info = path.stat()

        if path.is_dir():
            manifest["entries"].append(
                {
                    "path": relative,
                    "type": "directory",
                    "mtime_ns": info.st_mtime_ns,
                    "mode": stat.S_IMODE(info.st_mode),
                }
            )
            continue

        raw = path.read_bytes()
        chunk_ids: list[str] = []
        for chunk in fastcdc_chunks(raw):
            transformed, transforms, recipe = transform_chunk(chunk)
            chunk_id = checksum_bytes(transformed)
            chunk_ids.append(chunk_id)
            chunk_table.setdefault(
                chunk_id,
                {
                    "payload": compress_chunk(transformed),
                    "checksum": checksum_bytes(transformed),
                    "raw_size": len(transformed),
                    "transforms": transforms,
                    "recipe": recipe,
                },
            )
        manifest["entries"].append(
            {
                "path": relative,
                "type": "file",
                "mtime_ns": info.st_mtime_ns,
                "mode": stat.S_IMODE(info.st_mode),
                "size": len(raw),
                "checksum": checksum_bytes(raw),
                "chunks": chunk_ids,
            }
        )

    data_offset = 0
    for chunk_id, spec in sorted(chunk_table.items()):
        payload = spec["payload"]
        chunk_specs.append(
            ChunkSpec(
                chunk_id=chunk_id,
                offset=data_offset,
                raw_size=spec["raw_size"],
                compressed_size=len(payload),
                checksum=spec["checksum"],
                transforms=list(spec["transforms"]),
                recipe=str(spec["recipe"]),
            )
        )
        data_offset += len(payload)

    manifest["chunks"] = [
        {
            "id": spec.chunk_id,
            "offset": spec.offset,
            "raw_size": spec.raw_size,
            "compressed_size": spec.compressed_size,
            "checksum": spec.checksum,
            "transforms": spec.transforms,
            "recipe": spec.recipe,
        }
        for spec in chunk_specs
    ]

    manifest_bytes = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode("utf-8")
    header = HEADER_STRUCT.pack(MAGIC, 1, len(manifest_bytes))

    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("wb") as handle:
        handle.write(header)
        handle.write(manifest_bytes)
        for spec in chunk_specs:
            handle.write(chunk_table[spec.chunk_id]["payload"])


def read_archive(archive: Path) -> tuple[dict[str, object], bytes]:
    with archive.open("rb") as handle:
        header = handle.read(HEADER_STRUCT.size)
        if len(header) != HEADER_STRUCT.size:
            raise ValueError(f"{archive} is not a DVZ archive")
        magic, version, manifest_length = HEADER_STRUCT.unpack(header)
        if magic != MAGIC:
            raise ValueError(f"{archive} is not a DVZ archive")
        if version != 1:
            raise ValueError(f"Unsupported DVZ version: {version}")
        if manifest_length > MAX_MANIFEST_BYTES:
            raise ValueError("Manifest exceeds the supported reference limit")
        manifest_bytes = handle.read(manifest_length)
        if len(manifest_bytes) != manifest_length:
            raise ValueError("Archive ended before the manifest was fully read")
        manifest = json.loads(manifest_bytes.decode("utf-8"))
        payload = handle.read()
    return manifest, payload


def build_chunk_map(manifest: dict[str, object], payload: bytes) -> dict[str, bytes]:
    chunk_map: dict[str, bytes] = {}
    for chunk in manifest["chunks"]:
        offset = chunk["offset"]
        compressed_size = chunk["compressed_size"]
        raw_size = chunk["raw_size"]
        if raw_size > MAX_CHUNK_BYTES:
            raise ValueError(f"Chunk raw size exceeds the supported reference limit: {chunk['id']}")
        if offset + compressed_size > len(payload):
            raise ValueError(f"Chunk payload is truncated for {chunk['id']}")
        encoded = payload[offset:offset + compressed_size]
        decompressor = lzma.LZMADecompressor()
        raw = decompressor.decompress(encoded, max_length=raw_size + 1)
        if len(raw) != raw_size or not decompressor.eof:
            raise ValueError(f"Chunk size mismatch for {chunk['id']}")
        if checksum_bytes(raw) != chunk["checksum"]:
            raise ValueError(f"Checksum mismatch for chunk {chunk['id']}")
        chunk_map[chunk["id"]] = restore_chunk(raw, chunk.get("transforms", []), chunk.get("recipe", ""))
    return chunk_map


def extract_archive(archive: Path, destination: Path) -> None:
    manifest, payload = read_archive(archive)
    chunk_map = build_chunk_map(manifest, payload)
    destination.mkdir(parents=True, exist_ok=True)

    for entry in manifest["entries"]:
        relative = entry["path"]
        target = safe_destination_path(destination, relative)
        if entry["type"] == "directory":
            target.mkdir(parents=True, exist_ok=True)
            try:
                os.utime(target, ns=(entry["mtime_ns"], entry["mtime_ns"]), follow_symlinks=False)
            except (NotImplementedError, ValueError, OSError):
                os.utime(target, ns=(entry["mtime_ns"], entry["mtime_ns"]))
            continue

        target.parent.mkdir(parents=True, exist_ok=True)
        assembled = b"".join(chunk_map[chunk_id] for chunk_id in entry["chunks"])
        if checksum_bytes(assembled) != entry["checksum"]:
            raise ValueError(f"File checksum mismatch for {relative}")
        target.write_bytes(assembled)
        os.utime(target, ns=(entry["mtime_ns"], entry["mtime_ns"]))


def verify_archive(archive: Path) -> None:
    manifest, payload = read_archive(archive)
    chunk_map = build_chunk_map(manifest, payload)
    for entry in manifest["entries"]:
        if entry["type"] != "file":
            continue
        assembled = b"".join(chunk_map[chunk_id] for chunk_id in entry["chunks"])
        if checksum_bytes(assembled) != entry["checksum"]:
            raise ValueError(f"File checksum mismatch for {entry['path']}")


def default_archive_path(source: Path) -> Path:
    parent = source.parent if str(source.parent) else Path.cwd()
    candidate = parent / f"{source.name}.dvzr"
    if not candidate.exists():
        return candidate
    for suffix in range(2, 1000):
        candidate = parent / f"{source.name} ({suffix}).dvzr"
        if not candidate.exists():
            return candidate
    raise ValueError("Unable to determine an output archive path")


def default_extract_path(archive: Path) -> Path:
    stem = archive.stem or "extracted"
    return archive.parent / stem


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Reference DVZ compressor")
    subparsers = parser.add_subparsers(dest="command", required=True)

    compress_parser = subparsers.add_parser("compress")
    compress_parser.add_argument("source", type=Path)
    compress_parser.add_argument("archive", type=Path, nargs="?")

    extract_parser = subparsers.add_parser("extract")
    extract_parser.add_argument("archive", type=Path)
    extract_parser.add_argument("destination", type=Path, nargs="?")

    verify_parser = subparsers.add_parser("verify")
    verify_parser.add_argument("archive", type=Path)

    args = parser.parse_args(argv)

    try:
        if args.command == "compress":
            archive = args.archive or default_archive_path(args.source)
            archive_tree(args.source, archive)
            print(f"Created {archive}")
        elif args.command == "extract":
            destination = args.destination or default_extract_path(args.archive)
            extract_archive(args.archive, destination)
            print(f"Extracted to {destination}")
        elif args.command == "verify":
            verify_archive(args.archive)
            print(f"Verified {args.archive}")
        else:
            parser.error(f"Unknown command: {args.command}")
    except Exception as exc:  # pragma: no cover - CLI surface
        print(str(exc), file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
