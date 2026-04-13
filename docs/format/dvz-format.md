# DVZ Format

`.dvz` is a deterministic archive container optimized for size-first compression and future backend evolution.

## Design Goals

- Beat common `7z` settings on mixed large datasets.
- Preserve byte-exact file content.
- Keep backend swaps forward-readable by stamping archive metadata.
- Make failed writes detectable and incomplete archives untrusted.

## Container Layout

1. Fixed header
2. Serialized manifest
3. Block payload region

## Header

| Field | Size | Notes |
|------|------|-------|
| Magic | 4 bytes | `DVZ1` |
| Format version | 2 bytes | Current value: `1` |
| Header size | 2 bytes | Allows future extension |
| Manifest byte length | 8 bytes | Length of serialized manifest |
| Payload byte length | 8 bytes | Length of the block region |
| Manifest checksum | 16 bytes | DevZip v1 content fingerprint over the manifest bytes |

## Manifest

The manifest is serialized deterministically and contains:

- source root name
- backend identifier and version
- archive creation timestamp
- deterministic ordering flag
- file and directory entries
- block descriptors
- transform descriptors

### Entry Fields

- archive path using `/` separators
- entry kind: `file` or `directory`
- size in bytes
- modified timestamp in nanoseconds
- Windows attributes bitmask
- block references for file entries

### Block Fields

- stable block id
- raw size
- encoded size
- payload offset
- payload checksum
- transform pipeline applied to the block

## Determinism Rules

- Paths are sorted case-insensitively by normalized archive path.
- Directory entries are emitted before child file entries.
- Metadata serialization uses little-endian binary fields and length-prefixed UTF-8 strings.
- Identical inputs with identical backend settings should produce identical manifests.

## Integrity Rules

- The final archive is written to a temporary path and atomically renamed on success.
- Each payload block records a checksum over decoded bytes.
- The manifest checksum is validated before extraction.
- Unknown transform ids are fatal for extraction.

### DevZip v1 Content Fingerprint

The current native implementation uses a deterministic 16-byte fingerprint built from two FNV-1a lanes with different seeds. It is an internal archive integrity primitive for v1, not a cryptographic hash and not a cross-tool compatibility promise outside this repository's native implementation.

## Compatibility Rules

- A newer backend may read older format versions only if the manifest declares support.
- Archives must record backend name and backend version separately from format version.
- Format readers must reject unsupported major versions before touching payload data.

## Reserved Future Extensions

- encryption envelope
- alternate compression backends
- resumable solid blocks
- sparse file restoration
- alternate data stream preservation
