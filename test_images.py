import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

exe = ""
def fixture(shift=9):
    sector = 1 << shift
    count, heap, volume = 64, 25, 89
    data = bytearray(volume * sector)
    b = bytearray(12 * sector)
    b[:11] = b'\xeb\x76\x90EXFAT   '
    struct.pack_into('<QIIIIIIHH', b, 72, volume, 24, 1, heap, count, 2, 123, 0x100, 0)
    b[108:113] = bytes([shift, 0, 1, 0x80, 5])
    struct.pack_into('<H', b, 510, 0xaa55)
    for i in range(1, 9):
        struct.pack_into('<I', b, (i+1)*sector-4, 0xaa550000)
    checksum(b, sector)
    data[:12*sector] = b
    data[12*sector:24*sector] = b
    struct.pack_into('<II', data, 24*sector, 0xfffffff8, 0xffffffff)
    for c in (2, 3, 4):
        struct.pack_into('<I', data, 24*sector+c*4, 0xffffffff)
    data[heap*sector] = 0x81
    struct.pack_into('<IQ', data, heap*sector+20, 3, 8)
    data[heap*sector+32] = 0x82
    table = struct.pack('<HH', 0xffff, 97)
    table += struct.pack('<26H', *range(65, 91))
    table += struct.pack('<HHH', 0xffff, 65412, 0xffff)
    struct.pack_into('<I', data, heap*sector+36, checksum32(table))
    struct.pack_into('<IQ', data, heap*sector+52, 4, len(table))
    data[(heap+2)*sector:(heap+2)*sector+len(table)] = table
    data[(heap+1)*sector] = 7
    return data

def checksum32(data):
    value = 0
    for byte in data:
        value = (((value >> 1) | (value << 31)) + byte) & 0xffffffff
    return value


def sum16(data, skip=()):
    value = 0
    for i, byte in enumerate(data):
        if i not in skip:
            value = (((value >> 1) | (value << 15)) + byte) & 0xffff
    return value


def file_set(name='hello.txt', first=5, length=512, flags=3, directory=False):
    encoded = name.encode('utf-16-le')
    characters = len(encoded)//2
    names = (characters+14)//15
    entries = bytearray((2+names)*32)
    entries[0:2] = bytes([0x85, 1+names])
    struct.pack_into('<H', entries, 4, 0x10 if directory else 0x20)
    entries[32:36] = bytes([0xc0, flags, 0, characters])
    struct.pack_into('<H', entries, 36, sum16(name.upper().encode('utf-16-le')))
    struct.pack_into('<Q', entries, 40, length)
    struct.pack_into('<IQ', entries, 52, first, length)
    for i in range(names):
        entries[(2+i)*32] = 0xc1
        part = encoded[i*30:(i+1)*30]
        entries[(2+i)*32+2:(2+i)*32+2+len(part)] = part
    struct.pack_into('<H', entries, 2, sum16(entries, (2, 3)))
    return entries


def with_files(*entries):
    data = fixture()
    position = 25*512+64
    for entry in entries:
        data[position:position+len(entry)] = entry
        position += len(entry)
    return data


def allocated(data, *clusters):
    for cluster in clusters:
        index = cluster-2
        data[26*512+index//8] |= 1 << (index%8)
    return data


def fat_link(data, cluster, next_cluster):
    struct.pack_into('<I', data, 24*512+cluster*4, next_cluster)
    return data


def checksum(b, sector):
    value = 0
    for i in range(11*sector):
        if i not in (106, 107, 112):
            value = (((value >> 1) | (value << 31)) + b[i]) & 0xffffffff
    for i in range(11*sector, 12*sector, 4):
        struct.pack_into('<I', b, i, value)

def boots(data, mutate, sector=512):
    for base in (0, 12*sector):
        b = data[base:base+12*sector]
        mutate(b)
        checksum(b, sector)
        data[base:base+12*sector] = b

cases = 0


def main():
    global exe, cases
    exe = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp)/'volume.img'
        def run(data, args=(), ok=True, contains=''):
            global cases
            path.write_bytes(data)
            before = hashlib.sha256(path.read_bytes()).digest()
            command = [exe, 'plan' if args else 'inspect', str(path), *args]
            p = subprocess.run(command, capture_output=True, text=True)
            assert (p.returncode == 0) == ok, (command, p.stdout, p.stderr)
            assert contains in p.stdout+p.stderr, (contains, p.stdout, p.stderr)
            assert before == hashlib.sha256(path.read_bytes()).digest(), 'image modified'
            cases += 1
        run(fixture(), contains='Bitmap tail boundary bytes: 14336')
        run(fixture(12), contains='Cluster bytes: 4096')
        run(fixture(), ['16384'], contains='Candidate removed clusters: 57')
        run(fixture(), ['14336'])
        for target in ('13824', '16385', '45568', '-1', '18446744073709551616', ''):
            run(fixture(), [target], ok=False)
        data = fixture(); data[120] ^= 1
        run(data, ok=False, contains='checksum')
        data = fixture(); data[12*512+120] ^= 1
        run(data, ok=False, contains='checksum')
        data = fixture(); boots(data, lambda b: struct.pack_into('<H', b, 106, 2))
        run(data, ok=False, contains='volume flags')
        data = fixture(); boots(data, lambda b: b.__setitem__(110, 2))
        run(data, ok=False, contains='one FAT')
        run(fixture()[:-1], ok=False, contains='truncated')
        data = fixture(); struct.pack_into('<I', data, 24*512+8, 2)
        run(data, ok=False, contains='cycle')
        data = fixture(); struct.pack_into('<I', data, 24*512+12, 3)
        run(data, ok=False, contains='cycle')
        data = fixture(); data[26*512] = 6
        run(data, ok=False, contains='marked free')
        data = fixture(); data[26*512+7] = 128
        run(data, ['16384'], ok=False, contains='not owned')
        data = fixture(); boots(data, lambda b: struct.pack_into('<I', b, 92, 0xffffffff))
        run(data, ok=False, contains='geometry')
        # Exercise differing valid backup fields as a conservative rejection.
        data = fixture(); b = data[6144:12288]; struct.pack_into('<I', b, 100, 42); checksum(b, 512); data[6144:12288] = b
        run(data, ok=False, contains='disagree')
        # A contiguous stream's FAT entries are deliberately irrelevant.
        run(allocated(with_files(file_set()), 5), contains='Verified files: 1')
        run(with_files(file_set(first=0, length=0, flags=1)), contains='Verified files: 1')
        run(with_files(file_set(first=0, length=0, flags=3)), ok=False, contains='empty stream')
        run(with_files(file_set()), ok=False, contains='marked free')
        run(allocated(with_files(file_set(length=1024)), 5), ok=False, contains='marked free')
        run(allocated(with_files(file_set(), file_set('other.txt')), 5), ok=False, contains='cross-linked')
        run(allocated(with_files(file_set(), file_set('HELLO.TXT', first=6)), 5, 6), ok=False, contains='duplicate')
        data = allocated(with_files(file_set(flags=1, length=1024)), 5, 7)
        fat_link(data, 5, 7); fat_link(data, 7, 0xffffffff)
        run(data, contains='Verified files: 1')
        data = allocated(with_files(file_set(flags=1, length=1024)), 5, 7)
        fat_link(data, 5, 0xffffffff)
        run(data, ok=False, contains='shorter')
        data = allocated(with_files(file_set(flags=1)), 5, 7)
        fat_link(data, 5, 7); fat_link(data, 7, 0xffffffff)
        run(data, ok=False, contains='longer')
        data = allocated(with_files(file_set(flags=1, length=1024)), 5)
        fat_link(data, 5, 5)
        run(data, ok=False, contains='cross-linked')
        data = allocated(with_files(file_set('child', directory=True)), 5, 6)
        child = file_set('nested.txt', first=6)
        data[28*512:28*512+len(child)] = child
        run(data, contains='Verified subdirectories: 1')
        data = allocated(with_files(file_set('child', directory=True)), 5)
        child = file_set('loop', first=5, directory=True)
        data[28*512:28*512+len(child)] = child
        run(data, ok=False, contains='cross-linked')
        data = allocated(with_files(file_set()), 5); data[25*512+64+10] ^= 1
        run(data, ok=False, contains='set checksum')
        entry = file_set(); entry[36] ^= 1; struct.pack_into('<H', entry, 2, sum16(entry, (2, 3)))
        run(allocated(with_files(entry), 5), ok=False, contains='name hash')
        entry = file_set(); struct.pack_into('<Q', entry, 40, 513); struct.pack_into('<H', entry, 2, sum16(entry, (2, 3)))
        run(allocated(with_files(entry), 5), ok=False, contains='valid data length')
        run(allocated(with_files(file_set(directory=True, length=1)), 5), ok=False, contains='directory data length')
        data = fixture(); data[27*512] ^= 1
        run(data, ok=False, contains='up-case table checksum')
        data = fixture(); data[25*512+32] = 2
        run(data, ok=False, contains='missing up-case')
        data = fixture(); data[25*512+64] = 0xa0
        run(data, ok=False, contains='unsupported in-use')
        data = fixture(); data[25*512+64] = 0xc0
        run(data, ok=False, contains='unsupported in-use')
        data = allocated(fixture(), 5)
        run(data, ok=False, contains='not owned')
        data = with_files(file_set('a'*255))
        # A file entry set spans a second root-directory cluster.
        tail = data[25*512+512:25*512+64+19*32]
        data[25*512+512:25*512+64+19*32] = bytes(len(tail))
        data[29*512:29*512+len(tail)] = tail
        fat_link(data, 2, 6); fat_link(data, 6, 0xffffffff); allocated(data, 6)
        # The set overwrote the bitmap region while it was constructed; restore it.
        data[26*512:27*512] = bytes(512); allocated(data, 2, 3, 4, 5, 6)
        run(data, contains='Verified files: 1')
        data = fixture(); struct.pack_into('<I', data, 24*512, 0)
        run(data, ok=False, contains='reserved FAT')
        data = fixture(); struct.pack_into('<I', data, 24*512+4, 0)
        run(data, ok=False, contains='reserved FAT')

        def shrink(data, target, ok=True, existing=False):
            global cases
            source = Path(temp)/'source.img'
            destination = Path(temp)/'smaller.img'
            source.write_bytes(data)
            if destination.exists():
                destination.unlink()
            if existing:
                destination.write_bytes(b'preserve existing destination')
            original = hashlib.sha256(source.read_bytes()).digest()
            previous = destination.read_bytes() if existing else None
            result = subprocess.run([exe, 'shrink-copy', str(source), str(destination), str(target)],
                                    capture_output=True, text=True)
            assert (result.returncode == 0) == ok, (result.stdout, result.stderr)
            assert hashlib.sha256(source.read_bytes()).digest() == original
            if existing:
                assert destination.read_bytes() == previous
            if ok:
                assert destination.stat().st_size == target
                verified = subprocess.run([exe, 'inspect', str(destination)], capture_output=True, text=True)
                assert verified.returncode == 0, verified.stderr
                cases += 1
                return destination.read_bytes()
            cases += 1
            return None

        # An in-place request must be rejected without changing the source.
        source = Path(temp)/'same-path.img'
        source.write_bytes(fixture())
        original = hashlib.sha256(source.read_bytes()).digest()
        result = subprocess.run([exe, 'shrink-copy', str(source), str(source), '14336'],
                                capture_output=True, text=True)
        assert result.returncode != 0
        assert hashlib.sha256(source.read_bytes()).digest() == original
        cases += 1

        shrunk = shrink(fixture(), 14336)
        assert struct.unpack_from('<I', shrunk, 92)[0] == 3
        assert struct.unpack_from('<Q', shrunk, 25*512+24)[0] == 1
        assert shrunk[112] == 100
        shrink(fixture(12), 28*4096)
        shrink(fixture(), 16384, ok=False, existing=True)
        shrink(fixture(), 13824, ok=False)
        shrink(fixture(), 16385, ok=False)
        shrink(fixture(), 45568, ok=False)
        data = allocated(with_files(file_set()), 5)
        data[28*512:29*512] = bytes(range(256))*2
        shrunk = shrink(data, 29*512)
        assert shrunk[28*512:29*512] == data[28*512:29*512]
        # Independently parse and hash a payload from the newly generated image.
        from verify_image_hashes import verify
        manifest = Path(temp)/'manifest.csv'
        manifest.write_text('Algorithm,Hash,Path\nSHA256,' + hashlib.sha256(data[28*512:29*512]).hexdigest() + ',F:\\hello.txt\n')
        result = verify(Path(temp)/'smaller.img', manifest, 'F')
        assert result['matched'] == 1 and not result['failures'], result
        cases += 1
        # Fragmented file contents survive copying with their FAT links unchanged.
        data = allocated(with_files(file_set(length=1024, flags=1)), 5, 7)
        fat_link(data, 5, 7); fat_link(data, 7, 0xffffffff)
        data[28*512:29*512] = b'A'*512
        data[30*512:31*512] = b'B'*512
        shrunk = shrink(data, 31*512)
        assert shrunk[28*512:29*512] == b'A'*512
        assert shrunk[30*512:31*512] == b'B'*512
        # A two-cluster allocation bitmap becomes one cluster after shrinking.
        large = bytearray(5064*512)
        small = fixture()
        large[:24*512] = small[:24*512]
        boots(large, lambda b: (struct.pack_into('<Q', b, 72, 5064),
                               struct.pack_into('<I', b, 84, 40),
                               struct.pack_into('<I', b, 88, 64),
                               struct.pack_into('<I', b, 92, 5000)))
        large[64*512:65*512] = small[25*512:26*512]
        large[66*512:67*512] = small[27*512:28*512]
        struct.pack_into('<Q', large, 64*512+24, 625)
        large[65*512] = 15
        struct.pack_into('<II', large, 24*512, 0xfffffff8, 0xffffffff)
        fat_link(large, 2, 0xffffffff); fat_link(large, 3, 5)
        fat_link(large, 4, 0xffffffff); fat_link(large, 5, 0xffffffff)
        shrunk = shrink(large, 191*512)
        assert struct.unpack_from('<I', shrunk, 92)[0] == 127
        assert struct.unpack_from('<Q', shrunk, 64*512+24)[0] == 16
        assert shrunk[65*512] == 7
        assert struct.unpack_from('<I', shrunk, 24*512+3*4)[0] == 0xffffffff
        assert struct.unpack_from('<I', shrunk, 24*512+5*4)[0] == 0
        assert struct.unpack_from('<I', shrunk, 24*512+129*4)[0] == 0
    print(f'{cases} image checks passed; every input remained unchanged')


if __name__ == '__main__':
    main()
