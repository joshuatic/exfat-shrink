# exfat-shrink

A small C17 CLI for exFAT shrinking, with separate source files and mandatory
SHA-256 verification. It supports offline image copies and experimental Windows
live partition shrinking. A Windows Clang Release build is approximately 47 KiB.

The current algorithm removes an **empty tail**. It does not relocate clusters:
free capacity alone does not guarantee a volume can shrink to a particular size.
Unsupported or inconsistent filesystem features are rejected before live writes.

## Build

Requirements: CMake 3.16+, a C17 compiler, and Python 3 for automated tests.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

For multi-configuration generators, add `--config Release` to the build and
`-C Release` to CTest. Use `-DBUILD_TESTING=OFF` for a production-only build.
Runtime dependencies depend on the compiler's C runtime configuration.

## Commands

```sh
exfat_shrink inspect volume.img
exfat_shrink plan volume.img 2147483648
exfat_shrink shrink-copy volume.img smaller.img 2147483648
exfat_shrink hashes volume.img
exfat_shrink compare-files volume.img smaller.img
```

Image inputs must be offline exFAT volumes starting at byte zero, rather than
whole-disk images with a partition table. Targets are decimal bytes, sector aligned,
smaller than the current volume, and large enough to retain every allocated cluster.
Copy mode creates a new regular file and never overwrites an existing output.
Windows denies competing source writers; POSIX locking is advisory.

Exit codes are 0 for success, 1 for rejected inputs or operation failures, and 2
for incorrect usage. Failed image outputs may remain and must not be used.

## Windows live mode

From an elevated terminal, substituting the intended drive and target size:

```powershell
exfat_shrink shrink-live X 2147483648 C:\new-recovery-directory
```

This command writes the selected filesystem and resizes its actual partition.
The recovery directory must be new, local, and on a different physical disk.
Supported layouts are an ordinary GPT basic-data partition with no special
attributes or a nonboot MBR type-7 partition. Targets must be 1 MiB aligned.

The tool locks and dismounts the volume, validates it, and hashes all file contents
directly from the drive. It plans metadata changes in a sector overlay, validates
and hashes the proposed filesystem, then saves original and replacement sectors
in a durable journal. The journal is reopened and verified before live writes.
After writing and flushing, direct filesystem and hash checks precede partition
resizing. The selected partition identity and neighboring partitions are checked.

**No volume images or file-data copies are created in live mode.** Recovery storage
is the metadata journal size plus 1 MiB for preflight, independent of file-content
size. Smaller clusters and larger removed FAT ranges can increase journal size.
The in-memory sector plan has a 256 MiB safety bound; oversized plans are rejected
before writes. Hashes and allocation maps also consume RAM. Hashing hundreds of
GB requires multiple complete reads and can take hours.

Keep `metadata-journal.bin`, `layout-before.bin`, and `state.txt` after an operation.
Process failures attempt rollback and verify restored sectors and disk layout.
Power-loss recovery is currently manual: automatic journal replay and restart
recovery are not implemented. The journal covers metadata this operation changes;
it cannot restore unrelated media damage or replace a file backup. This is an
experimental tool, not a proven general-purpose partition manager.

## Validation and integrity

Checks include both boot signatures/checksums and matching geometry, FAT capacity
and reserved entries, root and bitmap chains, allocation ownership, directory entry
checksums, names and name hashes, the up-case table, fragmented and contiguous
streams, cross-links, cycles, and unexplained allocations.

Supported filesystem format: exFAT revision 1.00, a single FAT, and clear volume
flags. Unknown in-use entries, two-FAT/TexFAT, vendor allocations, dirty volumes,
and recorded bad clusters are rejected. Optional fields and every possible format
variant are not comprehensively covered. This is not a repair utility.

Mandatory SHA-256 checks cover logical file contents, including zero bytes beyond
ValidDataLength. File identity digests bind UTF-16 names to directory ancestry.
Reports include whole matching files and logical bytes, changed/missing files, and
unexpected files. Any mismatch fails the operation. Deleted data, free space, and
cluster slack are not file contents. New image outputs use sparse storage for
zero-filled blocks.

## Tests

CTest runs 103 fixture and hash checks, including corruption rejection, independent
SHA-256 vectors, file preservation, 512/4096-byte sector overlays, unchanged tails,
and byte-exact metadata rollback.

For Windows device/partition tests, build with testing enabled and run from an
elevated PowerShell terminal:

```powershell
.\test_live_partition.ps1 -Python python
```

This test creates a disposable 32 MiB GPT VHD, formats its selected partition as
exFAT, and writes files with independent expected hashes. It exercises failures
after dirty marking, filesystem writes, and partition resizing, followed by a
successful shrink to 24 MiB. Each phase checks CHKDSK, expected file hashes,
partition identity and size, and an untouched neighboring partition. It uses
under 70 MiB of temporary image storage and removes images after success. Failed
runs retain diagnostics. Fault injection exists only in `exfat_fault_test`.

The no-copy transaction tests passed with a 46,480-byte journal. Earlier testing
also shrank a physical approximately 16 GiB partition to 2 GiB, preserving 1,057
files and all 1,053 independent manifest hashes, with CHKDSK passing. These results
do not establish reliability for arbitrary volumes or a 750 GB deployment.

Generated reports, disk images, recovery files, IDE state, and builds are ignored
by Git. No personal drive manifests or local test-result folders are included.

## Source layout

- `main.c`: CLI dispatch.
- `exfat.c` / `verify.c`: structural validation and file traversal.
- `sha256.c` / `file_hash.c`: SHA-256 and file comparison.
- `metadata.c`: shared shrink metadata transformation.
- `shrink.c`: offline image copy and verification.
- `patch.c`: bounded sector overlay.
- `live.c`: Windows locking, journal, partition resize, and rollback.
- `test_*.c`, `test_*.py`, `test_live_partition.ps1`: regression tests.
- `make_test_vhd.py`, `verify_image_hashes.py`: independent test utilities.

Next milestones are automatic crash recovery, broader format and fault coverage,
and cluster relocation with bounded storage and exhaustive fragmented-volume tests.
