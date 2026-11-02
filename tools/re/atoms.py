"""Find ShortGuid atoms stored in the AI.exe image dump (.data/.bss): every aligned 4-byte value that is a known
ShortGuid of a name. Prints RVA, guid, name. Usage: python -I atoms.py <image.bin> <guids.tsv> <out.tsv> [start_rva end_rva]
"""
import struct, sys

def main():
    img, tsv, out = sys.argv[1:4]
    lo = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x1200000
    hi = int(sys.argv[5], 16) if len(sys.argv) > 5 else 0x1900000
    names = {}
    for line in open(tsv, encoding="utf-8"):
        g, n = line.rstrip("\n").split("\t", 1)
        names[int(g, 16)] = n
    data = open(img, "rb").read()
    hi = min(hi, len(data))
    hits = 0
    with open(out, "w", encoding="utf-8") as f:
        for rva in range(lo, hi - 3, 4):
            v = struct.unpack_from("<I", data, rva)[0]
            if v > 0x10000 and v in names:
                f.write(f"{rva:07x}\t{v:08x}\t{names[v]}\n"); hits += 1
    print(f"{hits} atom hits in rva {lo:x}-{hi:x}")

if __name__ == "__main__":
    main()
