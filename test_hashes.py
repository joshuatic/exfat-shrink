"""Verify the C hash primitive and end-to-end preservation failure detection."""

import hashlib
from pathlib import Path
import random
import re
import struct
import subprocess
import sys
import tempfile

from test_images import allocated, fat_link, file_set, fixture, sum16, with_files

exe = str(Path(sys.argv[1]).resolve())
sha_exe = str(Path(sys.argv[2]).resolve())
checks = 0
rng = random.Random(42017)

# Compare to an independent implementation across padding and update boundaries.
vectors = [b"", b"abc", b"a" * 1_000_000]
vectors.extend(rng.randbytes(size) for size in
               (1, 2, 3, 55, 56, 57, 63, 64, 65, 112, 113, 114, 127, 128,
                129, 511, 512, 513, 4095, 4096, 65535, 65536, 65537))
for payload in vectors:
    result = subprocess.run([sha_exe], input=payload, capture_output=True, check=True)
    assert result.stdout.decode().strip() == hashlib.sha256(payload).hexdigest()
    checks += 1

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    source = root / "source.img"
    other = root / "other.img"

    def listing(data):
        global checks
        source.write_bytes(data)
        before = hashlib.sha256(data).digest()
        result = subprocess.run([exe, "hashes", str(source)], capture_output=True, text=True)
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert hashlib.sha256(source.read_bytes()).digest() == before
        checks += 1
        return re.findall(r"SHA256 ([0-9a-f]{64}) BYTES (\d+) ID ([0-9a-f]{64})", result.stdout)

    def compare(before, after, ok, expected):
        global checks
        source.write_bytes(before)
        other.write_bytes(after)
        result = subprocess.run([exe, "compare-files", str(source), str(other)],
                                capture_output=True, text=True)
        assert (result.returncode == 0) == ok, (result.stdout, result.stderr)
        assert expected in result.stdout, (expected, result.stdout, result.stderr)
        assert source.read_bytes() == before
        assert other.read_bytes() == after
        checks += 1

    data = allocated(with_files(file_set()), 5)
    payload = rng.randbytes(512)
    data[28*512:29*512] = payload
    records = listing(data)
    assert len(records) == 1 and records[0][0] == hashlib.sha256(payload).hexdigest()
    assert records[0][1] == "512"
    compare(data, data, True, "SHA-256 verified file bytes: 512 / 512")
    corrupt = bytearray(data)
    corrupt[28*512+42] ^= 1
    compare(data, corrupt, False, "Changed files: 1; missing files: 0; unexpected files: 0")

    empty = with_files(file_set(first=0, length=0, flags=1))
    records = listing(empty)
    assert records[0][0] == hashlib.sha256(b"").hexdigest()
    compare(empty, empty, True, "SHA-256 matched files: 1 / 1")

    # ValidDataLength defines a zero-filled logical tail, regardless of raw slack.
    entry = file_set()
    struct.pack_into("<Q", entry, 40, 3)
    struct.pack_into("<H", entry, 2, sum16(entry, (2, 3)))
    data = allocated(with_files(entry), 5)
    data[28*512:29*512] = b"abc" + b"X"*509
    records = listing(data)
    assert records[0][0] == hashlib.sha256(b"abc" + bytes(509)).hexdigest()
    slack_changed = bytearray(data)
    slack_changed[28*512+300] ^= 1
    compare(data, slack_changed, True, "SHA-256 verified file bytes: 512 / 512")

    # Fragmented, partially filled final clusters hash in logical stream order.
    data = allocated(with_files(file_set(length=700, flags=1)), 5, 7)
    fat_link(data, 5, 7)
    fat_link(data, 7, 0xFFFFFFFF)
    payload = rng.randbytes(700)
    data[28*512:29*512] = payload[:512]
    data[30*512:30*512+188] = payload[512:]
    records = listing(data)
    assert records[0][0] == hashlib.sha256(payload).hexdigest()

    # Renames cannot masquerade as the same file even when content is unchanged.
    renamed = bytearray(data)
    replacement = file_set("renamed.txt", length=700, flags=1)
    renamed[25*512+64:25*512+64+len(replacement)] = replacement
    compare(data, renamed, False, "Changed files: 0; missing files: 1; unexpected files: 1")
    compare(data, fixture(), False, "missing files: 1")
    compare(fixture(), data, False, "unexpected files: 1")

    # Directory order does not affect matching; names bind to their ancestry.
    first = file_set("a.txt", first=5)
    second = file_set("b.txt", first=6)
    before = allocated(with_files(first, second), 5, 6)
    after = allocated(with_files(second, first), 5, 6)
    before[28*512:29*512] = after[28*512:29*512] = b"A"*512
    before[29*512:30*512] = after[29*512:30*512] = b"B"*512
    compare(before, after, True, "SHA-256 matched files: 2 / 2")
    data = allocated(with_files(file_set("one", first=5, directory=True),
                                file_set("two", first=6, directory=True)), 5, 6, 7, 8)
    child = file_set("same.txt", first=7)
    data[28*512:28*512+len(child)] = child
    child = file_set("same.txt", first=8)
    data[29*512:29*512+len(child)] = child
    data[30*512:31*512] = b"A"*512
    data[31*512:32*512] = b"B"*512
    records = listing(data)
    assert len(records) == 2 and records[0][2] != records[1][2]
    compare(data, data, True, "SHA-256 matched files: 2 / 2")
    changed = bytearray(data)
    changed[31*512] ^= 1
    compare(data, changed, False, "SHA-256 verified file bytes: 512 / 1024")

    # Shrink must compute a source baseline and hash output before success.
    destination = root / "shrunk.img"
    source.write_bytes(data)
    result = subprocess.run([exe, "shrink-copy", str(source), str(destination), str(32*512)],
                            capture_output=True, text=True)
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert "SHA-256 matched files: 2 / 2" in result.stdout
    assert "SHA-256 verified file bytes: 1024 / 1024" in result.stdout
    assert result.stdout.index("Hashing every source file") < result.stdout.index("Reopening output")
    assert result.stdout.index("All file contents preserved") < result.stdout.index("Created smaller image")
    assert source.read_bytes() == data
    checks += 1

print(f"{checks} SHA-256 and file preservation checks passed")
