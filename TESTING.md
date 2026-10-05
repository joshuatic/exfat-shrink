# Test coverage

Tests use small deterministic fixtures and newly created disposable VHDs. Existing
physical data volumes are never modified. Image fixtures use temporary directories;
Windows tests detach on failure and delete images after success. Failed diagnostics
remain for investigation. Run live tests serially to avoid shared report filenames.

## Commands

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

From an elevated PowerShell terminal:

```powershell
.\test_live_partition.ps1 -PartitionStyle GPT
.\test_live_partition.ps1 -PartitionStyle MBR -OffsetSectors 4097
```

The GPT layout includes neighbors before and immediately after the selected
partition. MBR includes a following sentinel. Offset 4097 is 2 MiB + 512 bytes:
sector aligned but not MiB aligned. The harness obtains identities from its own
VHD before formatting or writing. Recovery-storage checks use a second small VHD.
Typical peak live image storage is about 100 MiB. Reports/journals are small.

## Matrix

| Area | Tested | Remaining limits |
| --- | --- | --- |
| Boot/geometry | 512/4096-byte sectors, 512-byte through 256 KiB clusters, signatures/checksums, backup mismatch, field mutations, minimum/current/unaligned targets; sparse 750 GiB geometry | Physical 512e/4Kn hardware and other optional fields |
| Allocation | Empty/nearly empty, first/final retained and final volume clusters, crossing boundaries, stray bits, bitmap lengths/chains | Larger randomized allocation corpora |
| Files | Contiguous/fragmented; empty/subcluster/cluster/multicluster; Unicode/254-character names; 30 nesting levels; 5k/20k tiny files | Broader Unicode case folding |
| Boundaries | One-cluster, exact minimum, below minimum; 32-24-20-16 MiB live; direct 64-16 and repeated 64-48-32-24 MiB images | Relocation is unsupported |
| Integrity | Every logical file hashed; identities/sizes/ancestry; independent expected hashes; unchanged sources/tails; byte-exact overlay/rollback; CHKDSK/remount | Free/deleted data is not file content |
| Failures | Before/after journal; dirty marker; first/halfway metadata write; filesystem flush; partition resize; termination during rollback; mixed main boot sector | Physical power loss, controller effects, torn partition tables |
| Recovery | Termination after journal creation, first write, dirty flag, filesystem flush, resize and during rollback; mixed main/backup boot sectors; explicit restore; eight replays per stage; stale source-change refusal | GPT only, usable drive letter required; actual reboot pending |
| State persistence | 191 parsing/truncation checks; live missing/empty/truncated/unknown/committed state refusal; abandoned replacement file | Real interrupted atomic rename and stale identity binding |
| Torn sectors | 5,123 byte-mixture/corruption checks; live torn boot simulation with newer backup; full original-overlay validation | Byte values outside old/new images refused; physical tearing untested |
| Journal identity | SHA-256 binds saved layout; recomputed digest with wrong disk GUID refused; bounds/ordering checks; committed journal refused | No cryptographic authentication or exhaustive record fuzzing |
| Recovery storage | Full/read-only separate VHDs; missing directory; invalid output parent; existing output preserved | More driver failures and ACL combinations |
| Partition layouts | GPT basic data, MBR type 7, nonlast partition, immediate neighbor, unusual offset, special read-only GPT attribute, system/whole-disk rejection | Other GPT attributes and boot-volume detection |
| Unsupported inputs | Dirty flags, two FATs/TexFAT, bad clusters, unknown entries, FAT cycles/cross-links, directory/name checksum errors, broken up-case table | Not a complete repair/conformance suite |
| Windows | Open handle prevents lock; lock/dismount; remount/drive letters; actual size/GUID/offset; repeated shrinks | Additional forced driver dismount/flush failures |
| Physical same-SSD | F: on the same SATA SSD as D:, GPT, 512-byte logical/physical sectors; two complete 16 MiB shrink cycles with user hashes, CHKDSK, drive-letter reassignment and neighbor checks | One PC/controller; no reboot, 512e/4Kn or second-machine claim |
| Stress | 5k/20k tiny files, mixed sizes, 200 create/delete/recreate cycles, 120 randomized valid shrinks; 32 repeated CLI shrink cycles with bounded memory and released handles | No 750 GB workload or long-duration endurance proof |
| Parser fuzz | 120 deterministic metadata bit mutations, 40 random sector truncations; clean exit and unchanged source | Native coverage-guided fuzzing/sanitizers |
| CLI | Bad/missing args, negative/overflow/nondecimal sizes, suffix rejection, letter case, missing files, existing/same output, invalid parent, non-exFAT | Decimal byte sizes only by design |

Mutation cases may remain valid: success or clean rejection is acceptable, while
crashes, hangs, and input writes are forbidden. Specifically malformed field tests
require rejection. Counts represent command-level checks, not exhaustive proofs.
The harness retries transient PREPARING-state lock refusals up to three times; it
never retries a failed transaction that reached live writes.

## Results

All 6,394 automated checks passed: 103 original, 908 extended, 191 state,
5,123 torn-sector checks, five large sparse geometry checks and 64 endurance checks.
The final clean-source Clang Release build passed all eight suites. The final
MSVC Release production build and redirected `--help` passed; earlier MSVC testing
covered the six suites available then. This is a local fresh-source rehearsal,
not testing by an independent person or on a second PC.
Small workload images are at most 64 MiB logical. NTFS
sparse-source support was independently checked: 8 MiB logical consumed 64 KiB
allocated and every byte matched on readback. Source/output images are sparse.

GPT and MBR passed failure, rollback, independent file hashes, neighbor preservation,
repeated shrinking, CHKDSK and read-only remount. The unusual MBR offset passed.
GPT recovery additionally exercises journal/first-write termination and torn backup boot,
corrupt/truncated/partial/unsupported journal refusal, invalid-state refusal,
wrong-disk rejection, and duplicate replay. Independent expected hashes cover 74
surviving generated files. Full/read-only recovery storage, held-file refusal,
special GPT attributes, system-drive and whole-disk refusals also passed.

The empty 750 GiB sparse fixture allocated 983,040 bytes after flushing. Peak
working sets were below 10 MiB for inspect, plan and hashes. This validates large
geometry and low memory use for an empty filesystem, not a hundreds-of-GB workload.
The 32-cycle CLI endurance fixture stayed below 9 MiB and released its source and
output handles. Live repeated recovery encountered transient locking and state
publication failures; lock retries are restricted to the pre-write diagnostic,
and atomic state publication now retries only transient access/sharing failures.

The physical F: campaign completed 2,032 -> 2,016 -> 2,000 MiB, following an
initial CLI-verified 2,048 -> 2,032 MiB shrink. The two complete cycles checked
1,568 user files, including 514 generated files (about 34 MiB, 20 directory
levels and Unicode naming). The CLI separately hashed all 1,571 files including
Windows metadata, totaling 1,864,494,841 bytes. After deleting only the generated
directory, all 1,054 original user files matched the saved pre-test manifest.
External manifests exclude Windows-managed System Volume Information and recycle
metadata, which can change or remain locked after remount; direct CLI checks do
not exclude them while the volume is locked. Sampled live working set peaked at
10,510,336 bytes with at most 81 handles. This is bounded sampling on one workload,
not an exhaustive memory/driver leak proof. No generated physical test files remain.

Process termination loses process state while OS/storage stay running. The mixed
boot-sector write is a controlled byte-mixture simulation. Neither reproduces
controller cache loss or device power interruption. Recovery
compares current file contents before/after restoration; the original pre-crash hash
manifest is not persisted. These results do not establish reliability for arbitrary
volumes or a 750 GB deployment. Remaining limits above are deliberately unclaimed.
See [RELIABILITY.md](RELIABILITY.md) for the external campaigns and relocation gates.
