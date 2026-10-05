"""Deterministic small metadata, boundary, stress, fuzz and CLI checks."""
import hashlib
import os
from pathlib import Path
import random
import re
import struct
import subprocess
import sys
import tempfile
from test_images import fixture, checksum, checksum32, file_set
exe = str(Path(sys.argv[1]).resolve())
rng = random.Random(762913)
checks = 0
peak_logical = 0

def image(count=1024, shift=9, cshift=0, lengths=(), depth=0, fragmented=False):
    sector = 1 << shift
    cluster = sector << cshift
    fat_length = ((count + 2) * 4 + sector - 1) // sector
    heap = 24 + fat_length
    data = bytearray((heap + (count << cshift)) * sector)
    boot = fixture(shift)[:12 * sector]
    struct.pack_into('<QIIII', boot, 72, len(data) // sector, 24, fat_length, heap, count)
    boot[109] = cshift
    checksum(boot, sector)
    data[:12 * sector] = boot
    data[12 * sector:24 * sector] = boot
    struct.pack_into('<II', data, 24 * sector, 4294967288, 4294967295)
    table = struct.pack('<HH', 65535, 97) + struct.pack('<26H', *range(65, 91)) + struct.pack('<HHH', 65535, 65412, 65535)
    names = ['file-%05d.bin' % i for i in range(len(lengths))]
    if names:
        names[0] = '中文.txt'
    if len(names) > 1:
        names[1] = 'a' * 250 + '.bin'
    root_bytes = 64 + sum((len(file_set(n, first=0, length=0, flags=1)) for n in names))
    if depth:
        root_bytes += len(file_set('nested', directory=True))
    root_count = (root_bytes + 32 + cluster - 1) // cluster
    next_cluster = 2
    occupied = set()

    def allocate(number):
        nonlocal next_cluster
        first = next_cluster
        next_cluster += number
        assert next_cluster <= count + 2
        occupied.update(range(first, next_cluster))
        return first

    def offset(c):
        return heap * sector + (c - 2) * cluster

    def chain(first, number):
        for i in range(number):
            struct.pack_into('<I', data, 24 * sector + (first + i) * 4, first + i + 1 if i + 1 < number else 4294967295)
    root = allocate(root_count)
    chain(root, root_count)
    bitmap_length = (count + 7) // 8
    bitmap_count = (bitmap_length + cluster - 1) // cluster
    bitmap = allocate(bitmap_count)
    chain(bitmap, bitmap_count)
    upcase = allocate((len(table) + cluster - 1) // cluster)
    chain(upcase, (len(table) + cluster - 1) // cluster)
    data[offset(upcase):offset(upcase) + len(table)] = table
    entries = bytearray(64)
    entries[0] = 129
    entries[32] = 130
    struct.pack_into('<IQ', entries, 20, bitmap, bitmap_length)
    struct.pack_into('<I', entries, 36, checksum32(table))
    struct.pack_into('<IQ', entries, 52, upcase, len(table))
    digests = []
    for index, (name, length) in enumerate(zip(names, lengths)):
        number = (length + cluster - 1) // cluster
        payload = bytes((index * 17 + i * 13 + 3 & 255 for i in range(length)))
        digests.append(hashlib.sha256(payload).hexdigest())
        if not number:
            entries += file_set(name, first=0, length=0, flags=1)
            continue
        first = allocate(number * (2 if fragmented else 1))
        if fragmented:
            used = [first + i * 2 for i in range(number)]
            occupied.difference_update(set(range(first, first + number * 2)) - set(used))
            for i, c in enumerate(used):
                struct.pack_into('<I', data, 24 * sector + c * 4, used[i + 1] if i + 1 < number else 4294967295)
                part = payload[i * cluster:(i + 1) * cluster]
                data[offset(c):offset(c) + len(part)] = part
            entries += file_set(name, first=first, length=length, flags=1)
        else:
            data[offset(first):offset(first) + length] = payload
            entries += file_set(name, first=first, length=length)
    if depth:
        directories = [allocate(1) for _ in range(depth)]
        leaf = allocate(1)
        data[offset(leaf)] = 90
        digests.append(hashlib.sha256(b'Z').hexdigest())
        entries += file_set('nested', first=directories[0], length=cluster, directory=True)
        for i, c in enumerate(directories):
            entry = file_set('child', first=directories[i + 1], length=cluster, directory=True) if i + 1 < depth else file_set('leaf', first=leaf, length=1)
            data[offset(c):offset(c) + len(entry)] = entry
    data[offset(root):offset(root) + len(entries)] = entries
    for c in occupied:
        bit = c - 2
        data[offset(bitmap) + bit // 8] |= 1 << bit % 8
    minimum = heap * sector + (max(occupied) - 1) * cluster
    return (data, minimum, cluster, digests)

def sparse_write(path, data):
    global peak_logical
    peak_logical = max(peak_logical, len(data))
    with path.open('wb') as f:
        if os.name == 'nt':
            import ctypes
            import msvcrt
            returned = ctypes.c_ulong()
            handle = ctypes.c_void_p(msvcrt.get_osfhandle(f.fileno()))
            if not ctypes.windll.kernel32.DeviceIoControl(handle, 590020, None, 0, None, 0, ctypes.byref(returned), None):
                raise ctypes.WinError()
        f.truncate(len(data))
        for offset in range(0, len(data), 65536):
            block = data[offset:offset + 65536]
            if any(block):
                f.seek(offset)
                f.write(block)

def run(args, expected=0):
    global checks
    p = subprocess.run([exe, *map(str, args)], capture_output=True, text=True, timeout=90)
    assert p.returncode in (0, 1, 2), (args, p.returncode, p.stderr)
    if expected is not None:
        assert p.returncode == expected, (args, p.stdout, p.stderr)
    checks += 1
    return p
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    source = root / 'source.img'
    output = root / 'output.img'

    def case(data, target=None, digests=None):
        sparse_write(source, data)
        original = hashlib.sha256(data).digest()
        run(['inspect', source])
        if target is not None:
            output.unlink(missing_ok=True)
            run(['shrink-copy', source, output, target])
            run(['compare-files', source, output])
            assert output.stat().st_size == target
            if digests is not None:
                found = re.findall('^SHA256 ([0-9a-f]{64}) BYTES', run(['hashes', output]).stdout, re.M)
                assert sorted(found) == sorted(digests)
        assert hashlib.sha256(source.read_bytes()).digest() == original
    for shift, cshift in ((9, 0), (9, 3), (9, 6), (9, 7), (9, 8), (9, 9), (12, 0), (12, 4)):
        cluster = 1 << shift << cshift
        for fragmented in (False, True):
            data, minimum, cluster, digests = image(64 if cluster > 65536 else 256, shift, cshift, [0, 1, cluster - 1, cluster, cluster + 1], 30, fragmented)
            case(data, minimum, digests)
            sparse_write(source, data)
            run(['plan', source, minimum + (1 << shift)])
            run(['plan', source, minimum + cluster])
            for target in (minimum - (1 << shift), len(data), minimum + 1):
                run(['plan', source, target], 1)
            case(data, len(data) - cluster, digests)
    for files in (5000, 20000):
        data, minimum, cluster, digests = image(32768, lengths=[1] * files)
        case(data, minimum, digests)
    data, minimum, cluster, digests = image(129000, lengths=[1, 4096, 65536, 1048576], depth=10)
    assert len(data) < 64 * 1024 * 1024
    data.extend(bytes(64 * 1024 * 1024 - len(data)))
    for base in (0, 12 * 512):
        struct.pack_into('<Q', data, base + 72, len(data) // 512)
        boot = data[base:base + 12 * 512]
        checksum(boot, 512)
        data[base:base + 12 * 512] = boot
    case(data, 16 * 1024 * 1024, digests)
    sparse_write(source, data)
    for size in (48, 32, 24):
        output.unlink(missing_ok=True)
        run(['shrink-copy', source, output, size * 1024 * 1024])
        run(['compare-files', source, output])
        source.unlink()
        output.rename(source)
    for first, length in ((64, 512), (65, 512), (63, 1024), (64, 1024)):
        edge = fixture()
        entry = file_set('edge.bin', first=first, length=length)
        edge[25 * 512 + 64:25 * 512 + 64 + len(entry)] = entry
        payload = bytes((i * 19 + 1 & 255 for i in range(length)))
        start = (25 + first - 2) * 512
        edge[start:start + length] = payload
        for c in range(first, first + (length + 511) // 512):
            bit = c - 2
            edge[26 * 512 + bit // 8] |= 1 << bit % 8
        minimum = start + length
        case(edge, minimum if minimum < len(edge) else None, [hashlib.sha256(payload).hexdigest()])
        sparse_write(source, edge)
        run(['plan', source, minimum - 512], 1)
    for offset in (0, 3, 11, 72, 80, 84, 88, 92, 96, 104, 108, 109, 110, 510):
        damaged = fixture()
        damaged[offset] ^= 1
        sparse_write(source, damaged)
        run(['inspect', source], 1)
        assert source.read_bytes() == damaged
    for _ in range(120):
        lengths = [rng.randrange(0, 2049) for _ in range(rng.randrange(0, 8))]
        data, minimum, cluster, digests = image(256, lengths=lengths, depth=rng.randrange(0, 5), fragmented=bool(rng.randrange(2)))
        target = minimum + rng.randrange(0, (len(data) - minimum) // cluster) * cluster
        case(data, target, digests)
    pristine = fixture()
    for _ in range(120):
        data = bytearray(pristine)
        offset = rng.choice(list(range(512)) + list(range(25 * 512, 28 * 512)))
        data[offset] ^= 1 << rng.randrange(8)
        sparse_write(source, data)
        run(['inspect', source], None)
        assert source.read_bytes() == data
    for _ in range(40):
        size = rng.randrange(0, len(pristine) // 512) * 512
        sparse_write(source, pristine[:size])
        run(['inspect', source], 1)
        assert source.read_bytes() == pristine[:size]
    sparse_write(source, pristine)
    for args in ([], ['unknown'], ['inspect'], ['plan', source, '-1'], ['plan', source, '2GiB'], ['plan', source, '18446744073709551616'], ['plan', source, '0'], ['shrink-copy', source, source, 16384], ['inspect', root / 'missing.img'], ['recover-live', 'F:', root / 'missing'], ['recover-live', 'F', root / 'missing']):
        assert run(args, None).returncode != 0
    parent_file = root / 'not-a-directory'
    parent_file.write_bytes(b'keep')
    run(['shrink-copy', source, parent_file / 'child.img', 16384], 1)
    output.write_bytes(b'keep')
    run(['shrink-copy', source, output, 16384], 1)
    assert output.read_bytes() == b'keep'
    sparse_write(source, bytes(4096))
    run(['inspect', source], 1)
print(f'{checks} extended checks passed; peak single image {peak_logical / 1048576:.1f} MiB; temporary files removed.')
