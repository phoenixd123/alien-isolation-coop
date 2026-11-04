"""Decode coopnet AIC_PROBE=3 event logs: python decode_events.py events.txt ids.tsv [ids.tsv ...] --names guids.tsv --atoms atoms.tsv [--after <tick>]"""
import sys


def load(argv):
    names, ids = {}, {}
    opts = dict(zip(argv, argv[1:]))
    for line in open(opts["--names"], encoding="utf-8", errors="replace"):
        p = line.rstrip("\n").split("\t", 1)
        if len(p) == 2:
            names.setdefault(int(p[0], 16), p[1])
    for line in open(opts["--atoms"], encoding="utf-8", errors="replace"):
        p = line.rstrip("\n").split("\t")
        if len(p) >= 3:
            names[int(p[1], 16)] = p[2]
    skip = {opts.get("--names"), opts.get("--atoms"), opts.get("--after")}
    for f in argv[2:]:
        if f.startswith("--") or f in skip:
            continue
        for line in open(f, encoding="utf-8", errors="replace"):
            p = line.rstrip("\n").split("\t")
            if len(p) >= 5 and len(p[1]) == 8:
                ids.setdefault(int(p[1], 16), p)
    return names, ids, int(opts.get("--after", "0"))


def main(argv):
    names, ids, after = load(argv)
    nm = lambda v: names.get(v, "%08x" % v)
    def ent(v):
        p = ids.get(v)
        return "%s(%s)" % (p[3] or p[2], p[4]) if p else "%08x" % v
    for line in open(argv[1]):
        f = line.split()
        t, method, eid, typ = int(f[0]), int(f[1], 16), int(f[2], 16), int(f[3], 16)
        if t < after:
            continue
        path = []
        for pt in f[4:]:
            a, b = (int(x, 16) for x in pt.split(":"))
            if a or b:
                path.append(ent(a))
        print("%d %-24s %-40s %-22s  <- %s" % (t, nm(method), ent(eid), nm(typ), " / ".join(path)))


if __name__ == "__main__":
    main(sys.argv)
