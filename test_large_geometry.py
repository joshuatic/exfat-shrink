"""Validate large-volume geometry using sparse metadata, without copying data."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from test_images import fixture, checksum, checksum32


def make_image(path):
    sector = 512
    cluster = 131072
    count = (750 * 1024 ** 3) // cluster
    fat_length = ((count + 2) * 4 + sector - 1) // sector
    heap = 24 + fat_length
    length = heap * sector + count * cluster
    bitmap_length = (count + 7) // 8
    bitmap_clusters = (bitmap_length + cluster - 1) // cluster
    upcase = 3 + bitmap_clusters
    table = (struct.pack('<HH', 65535, 97) + struct.pack('<26H', *range(65, 91)) +
             struct.pack('<HHH', 65535, 65412, 65535))
    boot = fixture()[:12 * sector]
    struct.pack_into('<QIIII', boot, 72, length // sector, 24, fat_length, heap, count)
    boot[109] = 8
    checksum(boot, sector)
    fat = bytearray((upcase + 1) * 4)
    struct.pack_into('<III', fat, 0, 0xfffffff8, 0xffffffff, 0xffffffff)
    for c in range(3, upcase):
        struct.pack_into('<I', fat, c * 4, c + 1 if c + 1 < upcase else 0xffffffff)
    struct.pack_into('<I', fat, upcase * 4, 0xffffffff)
    root = bytearray(64)
    root[0], root[32] = 129, 130
    struct.pack_into('<IQ', root, 20, 3, bitmap_length)
    struct.pack_into('<I', root, 36, checksum32(table))
    struct.pack_into('<IQ', root, 52, upcase, len(table))
    bitmap = bytearray(bitmap_length)
    for bit in range(upcase - 1):
        bitmap[bit // 8] |= 1 << (bit % 8)
    with path.open('wb') as stream:
        if os.name == 'nt':
            import ctypes
            import msvcrt
            returned = ctypes.c_ulong()
            handle = ctypes.c_void_p(msvcrt.get_osfhandle(stream.fileno()))
            if not ctypes.windll.kernel32.DeviceIoControl(
                    handle, 0x900c4, None, 0, None, 0, ctypes.byref(returned), None):
                raise ctypes.WinError()
            # CRT truncation may zero-fill a huge sparse extension.
            if not ctypes.windll.kernel32.SetFilePointerEx(
                    handle, ctypes.c_longlong(length), None, 0):
                raise ctypes.WinError()
            if not ctypes.windll.kernel32.SetEndOfFile(handle):
                raise ctypes.WinError()
            stream.seek(0)
        else:
            stream.truncate(length)
        for offset, payload in ((0, boot), (12 * sector, boot), (24 * sector, fat),
                                (heap * sector, root), (heap * sector + cluster, bitmap),
                                (heap * sector + (upcase - 2) * cluster, table)):
            stream.seek(offset)
            stream.write(payload)
        stream.flush()
        os.fsync(stream.fileno())
    return heap * sector + (upcase - 1) * cluster


exe = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory() as directory:
    image = Path(directory) / 'large.img'
    minimum = make_image(image)
    if os.name == 'nt':
        import ctypes
        high = ctypes.c_ulong()
        allocated_size = ctypes.windll.kernel32.GetCompressedFileSizeW
        allocated_size.restype = ctypes.c_ulong
        low = allocated_size(str(image), ctypes.byref(high))
        allocated = (high.value << 32) | low
        assert 0 < allocated < 2 * 1024 * 1024, allocated
        print(f'Sparse image allocated {allocated} bytes for 750 GiB geometry.')
    for args, expected in ((['inspect', image], 0), (['plan', image, minimum], 0),
                           (['plan', image, minimum + 512], 0),
                           (['plan', image, minimum - 512], 1), (['hashes', image], 0)):
        with subprocess.Popen([exe, *map(str, args)], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True) as process:
            try:
                stdout, stderr = process.communicate(timeout=90)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
                raise
            assert process.returncode == expected, (args, stdout, stderr)
            if os.name == 'nt':
                class MemoryCounters(ctypes.Structure):
                    _fields_ = [('size', ctypes.c_ulong), ('faults', ctypes.c_ulong)] + [
                        (name, ctypes.c_size_t) for name in (
                            'peak_working', 'working', 'peak_paged', 'paged',
                            'peak_nonpaged', 'nonpaged', 'pagefile', 'peak_pagefile')]
                counters = MemoryCounters()
                counters.size = ctypes.sizeof(counters)
                if not ctypes.windll.psapi.GetProcessMemoryInfo(
                        ctypes.c_void_p(int(process._handle)), ctypes.byref(counters),
                        counters.size):
                    raise ctypes.WinError()
                assert counters.peak_working < 64 * 1024 * 1024, counters.peak_working
                print(f'{args[0]} peak working set: {counters.peak_working} bytes.')
    print('5 sparse 750 GiB geometry checks passed; no user files or data copy; temporary image removed.')
