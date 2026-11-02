"""Build a ShortGuid -> name dictionary from CathodeLib's info.dat (strings in the gzip payload) and
the strings inside AI.exe, using CathodeLib's ShortGuid algorithm (MIT, OpenCAGE/CathodeLib v1.0.0).
Usage: python -I guids.py <info.dat> <AI.exe> <out.tsv> [hexguid ...]
"""
import gzip, hashlib, re, sys

def short_guid(b: bytes) -> int:
    h = hashlib.sha1(b).digest()
    arr = bytes([h[3], h[2], h[1], h[0], h[7], h[6], h[5], h[4], h[11], h[10], h[9], h[8], h[15], h[14], h[13], h[12]])
    h2 = hashlib.sha1(arr.hex().upper().encode()).digest()
    return int.from_bytes(h2[:4], "little")

def strings(data: bytes, minlen=3):
    for m in re.finditer(rb"[\x20-\x7e]{%d,128}" % minlen, data):
        yield m.group()

def main():
    info, exe, out = sys.argv[1:4]
    names = set()
    raw = gzip.decompress(open(info, "rb").read())
    names.update(strings(raw))
    names.update(strings(open(exe, "rb").read(), 4))
    table = {}
    for n in names:
        table.setdefault(short_guid(n), n.decode("latin-1"))
    with open(out, "w", encoding="utf-8") as f:
        for g, n in sorted(table.items()):
            f.write(f"{g:08x}\t{n}\n")
    print(f"{len(names)} strings, {len(table)} guids")
    for q in sys.argv[4:]:
        g = int(q, 16)
        print(q, "->", table.get(g, "?"))
    # self-check against a known value is done by the caller

if __name__ == "__main__":
    main()
