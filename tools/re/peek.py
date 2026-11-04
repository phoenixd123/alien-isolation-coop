"""Read-only peek into a running test-copy process: python peek.py <pid> <hexaddr> <ndwords>"""
import ctypes
import struct
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = ctypes.c_void_p


def read(h, addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(ctypes.c_void_p(h), ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        return None
    return buf.raw[:got.value]


def open_ro(pid):
    return k32.OpenProcess(0x0010 | 0x0400, False, pid)   # VM_READ | QUERY_INFORMATION


if __name__ == "__main__":
    pid, addr, n = int(sys.argv[1]), int(sys.argv[2], 16), int(sys.argv[3])
    h = open_ro(pid)
    d = read(h, addr, n * 4)
    for i in range(0, len(d) // 4, 8):
        w = struct.unpack_from("<%dI" % min(8, len(d) // 4 - i), d, i * 4)
        print("%08x: %s" % (addr + i * 4, " ".join("%08x" % x for x in w)))
