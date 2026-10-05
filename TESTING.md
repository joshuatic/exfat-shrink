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
| Boot/geometry | 512/4096-byte sectors, 512-byte through 64 KiB clusters, signatures/checksums, backup mismatch, field mutations, minimum/current/unaligned targets | Other optional fields and cluster sizes |
| Allocation | Empty/nearly empty, first/final retained and final volume clusters, crossing boundaries, stray bits, bitmap lengths/chains | Larger randomized allocation corpora |
| Files | Contiguous/fragmented; empty/subcluster/cluster/multicluster; Unicode/254-character names; 30 nesting levels; 5k/20k tiny files | Broader Unicode case folding |
| Boundaries | One-cluster, exact minimum, below minimum; 32-24-20-16 MiB live; direct 64-16 and repeated 64-48-32-24 MiB images | Relocation is unsupported |
| Integrity | Every logical file hashed; identities/sizes/ancestry; independent expected hashes; unchanged sources/tails; byte-exact overlay/rollback; CHKDSK/remount | Free/deleted data is not file content |
| Failures | Before/after journal; dirty marker; first/halfway metadata write; filesystem flush; partition resize; termination during rollback | Physical power loss and torn writes |
| Recovery | Termination bypassing atexit at four stages; explicit restore; replay twice; checksum/header corruption and truncation refusal | GPT only, usable drive letter required |
| Journal identity | SHA-256 binds saved layout; recomputed digest with wrong disk GUID refused; bounds/ordering checks; committed journal refused | No cryptographic authentication or exhaustive record fuzzing |
| Recovery storage | Full/read-only separate VHDs; missing directory; invalid output parent; existing output preserved | More driver failures and ACL combinations |
| Partition layouts | GPT basic data, MBR type 7, nonlast partition, immediate neighbor, unusual offset, special read-only GPT attribute, system/whole-disk rejection | Other GPT attributes and boot-volume detection |
| Unsupported inputs | Dirty flags, two FATs/TexFAT, bad clusters, unknown entries, FAT cycles/cross-links, directory/name checksum errors, broken up-case table | Not a complete repair/conformance suite |
| Windows | Open handle prevents lock; lock/dismount; remount/drive letters; actual size/GUID/offset; repeated shrinks | Additional forced driver dismount/flush failures |
| Stress | 5k/20k tiny files, mixed sizes, 200 create/delete/recreate cycles, 120 randomized valid shrinks | No 750 GB workload or endurance proof |
| Parser fuzz | 120 deterministic metadata bit mutations, 40 random sector truncations; clean exit and unchanged source | Native coverage-guided fuzzing/sanitizers |
| CLI | Bad/missing args, negative/overflow/nondecimal sizes, suffix rejection, letter case, missing files, existing/same output, invalid parent, non-exFAT | Decimal byte sizes only by design |

Mutation cases may remain valid: success or clean rejection is acceptable, while
crashes, hangs, and input writes are forbidden. Specifically malformed field tests
require rejection. Counts represent command-level checks, not exhaustive proofs.
The harness retries transient PREPARING-state lock refusals up to three times; it
never retries a failed transaction that reached live writes.

## Results

All 913 automated checks passed: the original 103 plus 810 extended checks,
including direct 64-to-16 MiB shrinking. Largest image: 64 MiB logical. NTFS
sparse-source support was independently checked: 8 MiB logical consumed 64 KiB
allocated and every byte matched on readback. Source/output images are sparse.

GPT and MBR passed failure, rollback, independent file hashes, neighbor preservation,
repeated shrinking, CHKDSK and read-only remount. The unusual MBR offset passed.
GPT restart recovery passed four termination stages, corrupt/truncated journal and
wrong-disk rejection, and duplicate replay. Independent expected hashes cover 74
surviving generated files. Full/read-only recovery storage, held-file refusal,
special GPT attributes, system-drive and whole-disk refusals also passed.

Process termination loses process state while OS/storage stay running. It does not
simulate controller cache loss, write tearing or device power interruption. Recovery
compares current file contents before/after restoration; the original pre-crash hash
manifest is not persisted. These results do not establish reliability for arbitrary
volumes or a 750 GB deployment. Remaining limits above are deliberately unclaimed.
