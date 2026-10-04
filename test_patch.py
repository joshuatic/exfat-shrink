from pathlib import Path
import subprocess
import sys
import tempfile
from test_images import fixture, allocated, file_set, with_files

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    for shift in (9, 12):
        for content in (False, True):
            data = fixture(shift)
            if content:
                entry = file_set(length=1 << shift)
                data[25*(1 << shift)+64:25*(1 << shift)+64+len(entry)] = entry
                data[26*(1 << shift)] |= 8
                data[28*(1 << shift):29*(1 << shift)] = bytes((i*31 + 7) % 256 for i in range(1 << shift))
            source, expected = root/'source.img', root/'expected.img'
            source.write_bytes(data)
            expected.unlink(missing_ok=True)
            subprocess.run([sys.argv[1], 'shrink-copy', str(source), str(expected), str(45*(1 << shift))], check=True, capture_output=True)
            subprocess.run([sys.argv[2], str(source), str(expected)], check=True, capture_output=True)
print('4 metadata transaction fixtures passed (512/4096-byte sectors, empty/file data).')
