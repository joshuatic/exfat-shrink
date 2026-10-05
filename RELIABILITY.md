# Reliability work and release gates

The tool currently shrinks only a free tail. Allocated clusters beyond the target
are refused. Passing these tests does not establish safety for every exFAT volume.
Keep independently verified backups before any physical-volume operation.

## Automated work

The regression suite covers small sparse images, metadata rejection, file hashes,
fragmented streams, large directories, boundary sizes, and deterministic mutations.
Recovery-state tests require a complete recognized record, including its newline.
Torn-sector tests accept only byte values matching the journal's old or new sector;
unexpected values are refused. The dirty flag has a narrowly defined exception.
Recovery validates the reconstructed original filesystem before writing it.

The disposable GPT VHD harness simulates torn main and backup boot sectors.
It terminates without rollback, then runs recovery,
independent file hashes, CHKDSK, and duplicate replay. This is a deliberately mixed
sector written using aligned I/O, not a physical partial-sector write.

Malformed, missing, empty, unknown and committed state records are refused. An
unfinished `state.txt.new` is ignored when the original state remains valid.
Journal corruption, truncation, incomplete records, unsupported magic/version,
and a wrong disk identity are rejected. More journal permutations remain pending.
Source modification after recovery makes the old journal fail sector-version
validation. Eight recovery replays per interruption stage exercise repeated
locking. State publication retries transient access/sharing failures for a bounded
period; flush errors and exhausted retries still fail and preserve recovery data.
These retries improve handling of temporary interference; they are not a guarantee
against device loss or durable-storage failures.

The same-SSD physical test used the existing disposable F: partition on the SATA
SSD containing D:. Two complete shrink cycles passed independent user hashes,
CHKDSK, drive-letter reassignment and neighbor geometry checks. All 1,054 original
user files remained unchanged after generated files were removed. This covers
one 512/512-sector controller/Windows installation and about 1.8 GB of file data;
it does not cover the large real workload, second machine or reboot campaigns.

## External tests still required

Use disposable hardware or a VM whose virtual disk can be interrupted independently
of the machine running the test controller. Never power off a machine containing
the only recovery journal or only copy of important files.

For each interruption stage, record the disk/partition identity, target size,
original file manifest, journal location, build revision, and injected stage on
the controller before interruption. After reboot, collect the state and journal
unchanged, run `recover-live`, compare the original manifest independently, run
CHKDSK, and verify both neighboring partitions. Repeat recovery and verify again.
The current live harness tests process termination in the same Windows session;
it does not perform this reboot campaign.

Pending campaigns:

- Physical cuts during journal creation, metadata writes, rollback, before resize,
  and between resize and final state persistence.
- Real torn partition-table writes and primary/backup GPT disagreement. Do not
  assume the filesystem journal can repair a damaged partition table.
- SATA, NVMe, USB SSD/flash, HDD, different controllers, 512e and native 4Kn.
- A real approximately 750 GB volume with hundreds of GB of independently hashed
  files, memory and journal-space measurements, and long-duration verification.
- Device disappearance, drive-letter changes, competing writers, recovery-device
  loss, and recovery storage filling during writes.
- First/last usable GPT partitions, remaining MBR cases, ACL/non-admin cases,
  additional Windows versions, and independent testers following only README.

Sparse 750 GiB geometry checks validate arithmetic without a real large workload
or physical sector-layout claim. The software suite cannot substitute for the
hardware and reboot campaigns above.

## Cluster relocation implementation gate

Relocation is a separate feature, not an extra successful-shrink test. Keep live
mode's allocated-tail refusal until all of these steps pass:

1. Build a complete ownership map and deterministic move plan. Include files,
   directory streams, allocation bitmap and upcase storage. Reject unsupported
   entries, cross-links, cycles and insufficient free clusters below the target.
2. Implement relocation on a new output image first. Copy cluster data with bounded
   buffers; rewrite stream starting clusters, FAT links, directory-set checksums,
   allocation bitmap and boot geometry together. Validate all files against the
   untouched source, including contiguous-to-fragmented transitions.
3. Test files and directories crossing the boundary, bitmap movement, repeated
   shrinks, all stream layouts and every interrupted move. Preserve refusal in live
   mode until these offline cases pass.
4. Design a versioned durable move journal. Write and flush destination data before
   switching references; retain original allocation and references until commit.
   Prove recovery and rollback after every persistence boundary. Journal capacity
   must be planned before mutation and never depend on copying the entire volume.
5. Enable disposable live VHD relocation tests with independent original hashes,
   CHKDSK, neighbor checks and actual reboot recovery before physical-volume use.

The original pre-crash file manifest is not currently persisted by recovery.
Before claiming end-to-end crash integrity, store and verify it durably as part of
the transaction; comparing file contents immediately before/after recovery alone
cannot detect corruption that happened earlier.
