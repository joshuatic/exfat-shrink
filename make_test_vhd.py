"""Wrap a copied volume image in a new fixed VHD for read-only Windows checks.

Only the NEW VHD is written. The original volume image is opened read-only.
Uses Windows' VHD creation API; does not invent or manually encode a VHD footer.
"""

import argparse
import ctypes as ct
from ctypes import wintypes as wt
from pathlib import Path
import struct
import uuid
import zlib


class Guid(ct.Structure):
    _fields_ = [("data1", wt.DWORD), ("data2", wt.WORD), ("data3", wt.WORD),
                ("data4", ct.c_ubyte * 8)]


class StorageType(ct.Structure):
    _fields_ = [("device", wt.DWORD), ("vendor", Guid)]


class CreateParameters(ct.Structure):
    _fields_ = [("version", wt.DWORD), ("padding", wt.DWORD), ("unique", Guid), ("maximum", ct.c_uint64),
                ("block", wt.DWORD), ("sector", wt.DWORD),
                ("parent", wt.LPCWSTR), ("source", wt.LPCWSTR)]


def write_gpt(output, disk_size, partition_size, partition_start):
    sectors = disk_size // 512
    entries = bytearray(128 * 128)
    # A separate reserved sentinel partition checks neighbor preservation.
    for index, kind, first, last, name in (
        (0, 'e3c9e316-0b5c-4db8-817d-f92df00215ae', 34, partition_start - 1, 'Sentinel reserved'),
        (1, 'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7', partition_start,
         partition_start + partition_size // 512 - 1, 'exFAT live test'),
        (2, 'e3c9e316-0b5c-4db8-817d-f92df00215ae',
         partition_start + partition_size // 512, sectors - 34, 'Sentinel after'),
    ):
        base = index * 128
        entries[base:base+16] = uuid.UUID(kind).bytes_le
        entries[base+16:base+32] = uuid.uuid4().bytes_le
        struct.pack_into('<QQQ', entries, base+32, first, last, 0)
        encoded = name.encode('utf-16-le')
        entries[base+56:base+56+len(encoded)] = encoded
    entry_crc = zlib.crc32(entries)
    disk_id = uuid.uuid4().bytes_le
    for lba, alternate, entry_lba in ((1, sectors-1, 2), (sectors-1, 1, sectors-33)):
        header = bytearray(512)
        struct.pack_into('<8sIIIIQQQQ16sQIII', header, 0, b'EFI PART', 0x10000, 92,
                         0, 0, lba, alternate, 34, sectors-34, disk_id, entry_lba,
                         128, 128, entry_crc)
        struct.pack_into('<I', header, 16, zlib.crc32(header[:92]))
        output.seek(lba*512)
        output.write(header)
        output.seek(entry_lba*512)
        output.write(entries)


def make_vhd(image_path, destination, gpt=False, offset_sectors=2048):
    image_path = Path(image_path).resolve()
    destination = Path(destination).resolve()
    size = image_path.stat().st_size
    if offset_sectors < 2048:
        raise ValueError("Test partition offset must leave room for metadata")
    prefix = offset_sectors * 512

    if destination.exists():
        raise ValueError("Destination already exists")
    if size % 512 or size // 512 > 0xFFFFFFFF:
        raise ValueError("Unsupported MBR image size")

    storage = StorageType(2, Guid.from_buffer_copy(uuid.UUID("ec984aec-a0f9-47e9-901f-71415a66345b").bytes_le))
    disk_size = size + prefix + 1024 * 1024
    parameters = CreateParameters(1, 0, Guid(), disk_size, 0, 512, None, None)
    api = ct.WinDLL("virtdisk.dll")
    api.CreateVirtualDisk.argtypes = [ct.POINTER(StorageType), wt.LPCWSTR, wt.DWORD,
                                     ct.c_void_p, wt.DWORD, wt.DWORD,
                                     ct.POINTER(CreateParameters), ct.c_void_p,
                                     ct.POINTER(wt.HANDLE)]
    api.CreateVirtualDisk.restype = wt.DWORD
    handle = wt.HANDLE()
    result = api.CreateVirtualDisk(ct.byref(storage), str(destination), 0x00100000,
                                  None, 1, 0, ct.byref(parameters), None, ct.byref(handle))
    if result:
        raise ct.WinError(result)

    kernel = ct.WinDLL("kernel32.dll", use_last_error=True)
    kernel.CloseHandle.argtypes = [wt.HANDLE]
    kernel.CloseHandle.restype = wt.BOOL
    if not kernel.CloseHandle(handle):
        raise ct.WinError(ct.get_last_error())

    if destination.stat().st_size != disk_size + 512:
        raise ValueError("Unexpected fixed VHD size")

    with image_path.open("rb") as source, destination.open("r+b") as output:
        mbr = bytearray(512)
        struct.pack_into("<I", mbr, 440, int.from_bytes(uuid.uuid4().bytes[:4], "little"))
        struct.pack_into("<B3sB3sII", mbr, 446, 0, b"\xfe\xff\xff", 0xee if gpt else 7,
                         b"\xfe\xff\xff", 1 if gpt else prefix // 512,
                         disk_size // 512 - 1 if gpt else size // 512)
        if not gpt:
            struct.pack_into("<B3sB3sII", mbr, 462, 0, b"\xfe\xff\xff", 0xda,
                             b"\xfe\xff\xff", (prefix + size) // 512, 2048)
        mbr[510:512] = b"\x55\xaa"
        output.write(mbr)
        if gpt:
            write_gpt(output, disk_size, size, prefix // 512)
        output.seek(prefix)
        remaining = size

        while remaining:
            data = source.read(min(remaining, 1024 * 1024))
            if not data:
                raise ValueError("Short source read")
            output.write(data)
            remaining -= len(data)

        # Match the synthetic MBR's partition offset in the copied boot regions.
        source.seek(0)
        header = source.read(512)
        if header[3:11] != b"EXFAT   " or header[108] != 9:
            raise ValueError("This test wrapper currently supports 512-byte exFAT sectors")

        for base in (0, 12 * 512):
            source.seek(base)
            boot = bytearray(source.read(12 * 512))
            struct.pack_into("<Q", boot, 64, prefix // 512)
            checksum = 0
            for index in range(11 * 512):
                if index not in (106, 107, 112):
                    checksum = (((checksum >> 1) | (checksum << 31)) + boot[index]) & 0xFFFFFFFF
            for index in range(11 * 512, 12 * 512, 4):
                struct.pack_into("<I", boot, index, checksum)
            output.seek(prefix + base)
            output.write(boot)

    print(f"Created test wrapper: {destination}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image")
    parser.add_argument("destination")
    parser.add_argument("--gpt", action="store_true")
    parser.add_argument("--offset-sectors", type=int, default=2048)
    args = parser.parse_args()
    make_vhd(args.image, args.destination, args.gpt, args.offset_sectors)
