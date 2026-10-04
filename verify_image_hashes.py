"""Independently read an exFAT image and compare file contents with a CSV manifest.

Uses Python's hashlib, not the C analyzer. Never mounts or modifies the image.
This is test tooling, not a repair tool or a replacement for filesystem checks.
"""

import argparse
import csv
import hashlib
import json
import struct
from pathlib import Path, PureWindowsPath


def verify(image_path, manifest_path, drive):
    with open(image_path, "rb") as image:
        def read_at(offset, size):
            image.seek(offset)
            data = image.read(size)
            if len(data) != size:
                raise ValueError("Short image read")
            return data

        boot = read_at(0, 512)
        if boot[3:11] != b"EXFAT   ":
            raise ValueError("Not an exFAT volume image")

        sector = 1 << boot[108]
        cluster_bytes = sector << boot[109]
        fat, _, heap, count, root = struct.unpack_from("<5I", boot, 80)
        image_size = Path(image_path).stat().st_size

        def clusters(first, length=None, contiguous=False):
            if length == 0:
                if first:
                    raise ValueError("Allocated empty stream")
                return

            remaining = None if length is None else (length + cluster_bytes - 1) // cluster_bytes
            visited = set()
            current = first

            while current != 0xFFFFFFFF:
                if not 2 <= current < count + 2 or current in visited:
                    raise ValueError("Invalid or cyclic cluster chain")
                if remaining == 0:
                    raise ValueError("Overlong cluster chain")
                visited.add(current)
                yield current
                if remaining is not None:
                    remaining -= 1
                if contiguous:
                    if remaining == 0:
                        return
                    current += 1
                else:
                    current = struct.unpack("<I", read_at(fat * sector + current * 4, 4))[0]

            if remaining not in (None, 0):
                raise ValueError("Short cluster chain")

        def offset(cluster):
            position = (heap + (cluster - 2) * (cluster_bytes // sector)) * sector
            if position + cluster_bytes > image_size:
                raise ValueError("Cluster beyond image")
            return position

        def entries(first, length, contiguous):
            for cluster in clusters(first, length, contiguous):
                # Directories have whole-cluster allocations.
                data = read_at(offset(cluster), cluster_bytes)
                for position in range(0, len(data), 32):
                    yield data[position:position + 32]

        observed = {}
        directories = [("", root, None, False)]
        visited_directories = set()

        while directories:
            prefix, first, length, contiguous = directories.pop()
            if first in visited_directories:
                raise ValueError("Directory cycle")
            visited_directories.add(first)
            iterator = iter(entries(first, length, contiguous))

            for entry in iterator:
                if entry[0] == 0:
                    break
                if entry[0] != 0x85:
                    continue

                secondary = [next(iterator) for _ in range(entry[1])]
                if not secondary or secondary[0][0] != 0xC0:
                    raise ValueError("Missing stream extension")
                stream = secondary[0]
                encoded = b"".join(item[2:32] for item in secondary[1:] if item[0] == 0xC1)
                name = encoded[:stream[3] * 2].decode("utf-16-le")
                path = prefix + name
                stream_first = struct.unpack_from("<I", stream, 20)[0]
                valid_length = struct.unpack_from("<Q", stream, 8)[0]
                data_length = struct.unpack_from("<Q", stream, 24)[0]
                contiguous = bool(stream[1] & 2)

                if struct.unpack_from("<H", entry, 4)[0] & 0x10:
                    if data_length:
                        directories.append((path + "\\", stream_first, data_length, contiguous))
                    continue

                digest = hashlib.sha256()
                consumed = 0
                for cluster in clusters(stream_first, data_length, contiguous):
                    size = min(cluster_bytes, data_length - consumed)
                    initialized = min(size, max(0, valid_length - consumed))
                    if initialized:
                        digest.update(read_at(offset(cluster), initialized))
                    if initialized < size:
                        digest.update(bytes(size - initialized))
                    consumed += size

                key = path.casefold()
                if key in observed:
                    raise ValueError("Duplicate file path")
                observed[key] = digest.hexdigest().upper()

    checked = 0
    failures = []
    with open(manifest_path, newline="", encoding="utf-8-sig") as manifest:
        for row in csv.DictReader(manifest):
            if row["Algorithm"].upper() != "SHA256":
                raise ValueError("Only SHA256 manifests are supported")
            relative = str(PureWindowsPath(row["Path"]).relative_to(drive + ":\\"))
            actual = observed.get(relative.casefold())
            checked += 1
            if actual != row["Hash"].upper():
                failures.append({"path": row["Path"], "actual": actual, "expected": row["Hash"]})

    if not checked:
        raise ValueError("Empty manifest")

    return {"checked": checked, "matched": checked - len(failures), "image_files": len(observed),
            "failures": failures}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image")
    parser.add_argument("manifest")
    parser.add_argument("--drive", default="F")
    args = parser.parse_args()
    result = verify(args.image, args.manifest, args.drive)
    print(json.dumps(result, indent=2))
    raise SystemExit(bool(result["failures"]))
