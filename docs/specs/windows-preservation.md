# Windows Preservation Contract

This document defines the minimum v1 source-scanning and restore behavior for DevZip on Windows.

## Preserved in V1

- Relative path structure
- File bytes
- Empty directories
- Modified timestamps
- DOS file attributes
- Unicode file and directory names
- Long paths that `std::filesystem` or the Win32 long-path prefix can resolve

## Explicit V1 Exclusions

- Alternate data streams
- ACLs and ownership
- EFS metadata
- sparse file hole maps
- live file-change capture during compression

## Reparse Points and Junctions

- Directory reparse points are not followed by default.
- Reparse points are recorded as skipped items unless future versions gain explicit support.
- If follow-reparse behavior is enabled for internal testing, canonical target paths must be cycle-checked.

## Traversal Rules

- Inputs are scanned in deterministic order.
- Permission-denied paths fail the archive build unless the caller explicitly allows skip mode.
- Hidden files are included by default.
- Circular traversal is prevented by tracking canonical directory identities.

## Output Naming

- Default archive path is next to the dropped input.
- File input `example.iso` becomes `example.iso.dvz`.
- Directory input `photos` becomes `photos.dvz`.
- Name collisions append ` (2)`, ` (3)`, and so on before `.dvz`.

## Failure Behavior

- Failed scans do not produce a trusted final archive.
- Partial temporary files are deleted on failure or cancellation.
- Inaccessible paths surface a clear error containing the failing path.
