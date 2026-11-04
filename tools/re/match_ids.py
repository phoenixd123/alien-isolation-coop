"""Locate Cathode IDs in call_method samples (coopnet AIC_PROBE=2 -> coop_logs/samples_*.bin).

Usage: python match_ids.py <samples.bin> <ids.tsv> [<ids.tsv> ...] [--names guids.tsv] [--atoms atoms.tsv]

ids.tsv comes from `levelmod ids <COMMANDS.PAK>` (kind, hex dword, composite, name, type).
guids.tsv maps ShortGuid(name) -> name (tools/re/guids.py); atoms.tsv is rva, value, name.
For every dword position in the samples it counts which kind of ID shows up there, then
decodes each sample as method name + target entity, using the best positions.
"""
import struct
import sys
from collections import Counter, defaultdict

NDW = 1 + 3 + 1 + 1 + 24 + 768 + 64


def load_samples(path):
    data = open(path, "rb").read()
    out = []
    for i in range(len(data) // (NDW * 4)):
        d = struct.unpack_from("<%dI" % NDW, data, i * NDW * 4)
        s = {"self": d[0], "arg": d[1:4], "tvt": d[4], "mvt": d[5]}
        l1 = d[6:30]
        l2 = d[30:798]
        s["tobj"] = d[798:862]
        s["l1"] = [l1[k * 8:(k + 1) * 8] for k in range(3)]
        s["l2"] = [[l2[(k * 8 + j) * 32:(k * 8 + j + 1) * 32] for j in range(8)] for k in range(3)]
        out.append(s)
    return out


def main(argv):
    names, atoms, ids = {}, {}, {}
    files = [a for a in argv[1:] if not a.startswith("--")]
    opts = dict(zip(argv[1::1], argv[2::1]))
    if "--names" in opts:
        for line in open(opts["--names"], encoding="utf-8", errors="replace"):
            p = line.rstrip("\n").split("\t", 1)
            if len(p) == 2:
                names.setdefault(int(p[0], 16), p[1])
    if "--atoms" in opts:
        for line in open(opts["--atoms"], encoding="utf-8", errors="replace"):
            p = line.rstrip("\n").split("\t")
            if len(p) >= 3:
                atoms[int(p[1], 16)] = p[2]
    skip = {opts.get("--names"), opts.get("--atoms")}
    samples = load_samples(files[0])
    for f in files[1:]:
        if f in skip:
            continue
        for line in open(f, encoding="utf-8", errors="replace"):
            p = line.rstrip("\n").split("\t")
            if len(p) >= 5 and len(p[1]) == 8:
                ids.setdefault(int(p[1], 16), (p[0], p[2], p[3], p[4]))

    def kind(v):
        if v in ids:
            return "id:" + ids[v][0]
        if v in atoms or v in names:
            return "name"
        return None

    pos = defaultdict(Counter)
    for s in samples:
        for k in range(3):
            for j in range(8):
                kd = kind(s["l1"][k][j])
                if kd:
                    pos[("l1", k, j * 4)][kd] += 1
                for o in range(32):
                    kd = kind(s["l2"][k][j][o])
                    if kd:
                        pos[("l2", k, j * 4, o * 4)][kd] += 1
        for o, v in enumerate(s["tobj"]):
            kd = kind(v)
            if kd:
                pos[("tobj", o * 4)][kd] += 1
    print("samples: %d" % len(samples))
    print("positions where IDs or names appear (count of samples):")
    for p, c in sorted(pos.items(), key=lambda x: -sum(x[1].values()))[:40]:
        print("  %-22s %s" % ("/".join(str(x) if isinstance(x, int) and x < 3 else hex(x) if isinstance(x, int) else x for x in p), dict(c)))

    def label(v):
        if v in ids:
            k, comp, name, typ = ids[v]
            return "%s %s (%s) in %s" % (k, name, typ, comp)
        return atoms.get(v) or names.get(v) or "%08x" % v

    print("\nper sample: method (msg+0) -> target object dwords that are IDs")
    for s in samples[:200]:
        method = label(s["mvt"])
        hits = ["+%x=%s" % (o * 4, label(v)) for o, v in enumerate(s["tobj"]) if kind(v)]
        print("  %-28s %s" % (method, "; ".join(hits[:4]) or "-"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
