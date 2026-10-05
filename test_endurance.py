"""Repeated CLI transactions: memory bounds and released source/output handles."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from test_images import fixture


exe = str(Path(sys.argv[1]).resolve())
peak_memory = 0
if os.name == 'nt':
    import ctypes

    class MemoryCounters(ctypes.Structure):
        _fields_ = [('size', ctypes.c_ulong), ('faults', ctypes.c_ulong)] + [
            (name, ctypes.c_size_t) for name in (
                'peak_working', 'working', 'peak_paged', 'paged',
                'peak_nonpaged', 'nonpaged', 'pagefile', 'peak_pagefile')]


def run(args):
    global peak_memory
    with subprocess.Popen([exe, *map(str, args)], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True) as process:
        try:
            stdout, stderr = process.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise
        assert process.returncode == 0, (args, stdout, stderr)
        if os.name == 'nt':
            counters = MemoryCounters()
            counters.size = ctypes.sizeof(counters)
            if not ctypes.windll.psapi.GetProcessMemoryInfo(
                    ctypes.c_void_p(int(process._handle)), ctypes.byref(counters), counters.size):
                raise ctypes.WinError()
            peak_memory = max(peak_memory, counters.peak_working)
            assert counters.peak_working < 64 * 1024 * 1024


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    source = root / 'source.img'
    output = root / 'output.img'
    probe = root / 'probe.img'
    original = bytes(fixture())
    source.write_bytes(original)
    for cycle in range(32):
        run(['shrink-copy', source, output, 32 * 512])
        run(['compare-files', source, output])
        # Windows refuses these operations if a competing handle survives exit.
        source.rename(probe)
        probe.rename(source)
        output.unlink()
        assert source.read_bytes() == original
print(f'64 endurance command checks passed across 32 shrink cycles; '
      f'peak working set {peak_memory} bytes; handles released and temporary files removed.')
