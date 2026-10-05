# exfat-shrink

> **Experimental, extensively tested** exFAT shrinker with transactional live resizing and integrity verification.

A small C17 CLI for exFAT shrinking, with separate source files and mandatory
SHA-256 verification. It supports offline image copies and experimental Windows
live partition shrinking. A Windows Clang Release build is approximately 56 KiB.

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
The tested Windows binaries import `VCRUNTIME140.dll` and the Windows Universal
C runtime; those runtime components must be present on the destination PC.
On Windows, single-configuration builds place the CLI in `build\exfat_shrink.exe`;
Visual Studio Release builds place it in `build\Release\exfat_shrink.exe`.

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
For an interrupted GPT transaction, explicitly restore the original partition with:

```powershell
exfat_shrink recover-live X C:\transaction-recovery-directory
```

Recovery verifies the journal/layout SHA-256, disk and partition identities,
neighboring partitions, sector bounds and ordering, and the proposed original
filesystem before writing. It refuses committed transactions, unsupported journal
versions, or sector bytes outside the recorded old/new values (apart from the
defined dirty-flag allowance). It verifies restored
metadata and current file contents afterward. Repeat replay is supported. Recovery
is currently GPT-only and requires the volume to retain a usable drive letter;
it does not automatically run at startup or repair torn/unrecognized sector writes.
The journal checksum detects accidental corruption; it is not authentication.
Recovery checks current file contents before/after restoration, not a persisted
pre-crash file-hash baseline. The journal covers metadata this operation changes;
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

CTest runs the original 103 checks plus an extended deterministic suite, including corruption rejection, independent
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
about 100 MiB of temporary image storage when recovery-storage tests run and removes images after success. Failed
runs retain diagnostics. Fault injection exists only in `exfat_fault_test`.

The original no-copy transaction test used a 46,480-byte journal. Journals now
include a 32-byte checksum bound to the saved layout. See [TESTING.md](TESTING.md)
for the current coverage matrix and verification results. Earlier testing
also shrank a physical approximately 16 GiB partition to 2 GiB, preserving 1,057
files and all 1,053 independent manifest hashes, with CHKDSK passing. These results
do not establish reliability for arbitrary volumes or a 750 GB deployment.

The live harness accepts `-RecoveryReplays 8` for repeated recovery testing and
finds both single-configuration and Visual Studio Release executable paths.
`test_physical_partition.ps1 -DriveLetter F` is specific to this development PC's
small `EXFATTEST` partition on the same SSD as D:. It never formats or creates a
partition, preserves user files, adds about 34 MiB of test data, shrinks in 16 MiB
steps, checks hashes/CHKDSK/neighbors and reassigns the drive letter. It requires
administrator access and a recovery directory on a different physical disk.
Do not use it as a generic fresh-user disk-creation procedure; use the disposable
VHD harness above for that purpose.

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
- `recovery_state.c` / `recovery_sector.c`: recovery state and torn-sector validation.
- `test_*.c`, `test_*.py`, `test_live_partition.ps1`: regression tests.
- `make_test_vhd.py`, `verify_image_hashes.py`: independent test utilities.

Next milestones are durable original hash manifests, recovery without a drive
letter, actual reboot/power-loss campaigns, and cluster relocation with bounded
storage. [RELIABILITY.md](RELIABILITY.md) tracks the remaining release gates.
