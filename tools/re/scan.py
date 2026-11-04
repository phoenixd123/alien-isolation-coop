"""Read-only scan of a running process for a byte pattern: python scan.py <pid> <hex bytes> [max]"""
import ctypes
import ctypes.wintypes as wt
import sys

import peek


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p), ("AllocationProtect", wt.DWORD),
                ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD), ("Protect", wt.DWORD), ("Type", wt.DWORD)]


def regions(h):
    addr, mbi = 0, MBI()
    while addr < 0x7FFF0000 and peek.k32.VirtualQueryEx(ctypes.c_void_p(h), ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        base, size = mbi.BaseAddress or 0, mbi.RegionSize
        if mbi.State == 0x1000 and mbi.Protect in (0x04, 0x02, 0x40, 0x20, 0x08) and not (mbi.Protect & 0x100):
            yield base, size
        addr = base + size


def scan(h, pat, limit=50):
    out = []
    for base, size in regions(h):
        for off in range(0, size, 1 << 24):
            n = min(1 << 24, size - off)
            d = peek.read(h, base + off, n)
            if not d:
                continue
            i = d.find(pat)
            while i >= 0:
                if (base + off + i) % 4 == 0:
                    out.append(base + off + i)
                    if len(out) >= limit:
                        return out
                i = d.find(pat, i + 1)
    return out


if __name__ == "__main__":
    h = peek.open_ro(int(sys.argv[1]))
    for a in scan(h, bytes.fromhex(sys.argv[2]), int(sys.argv[3]) if len(sys.argv) > 3 else 50):
        print("%08x" % a)
