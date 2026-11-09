#!/usr/bin/env python3
"""Find the decrypted prod/conf/fur configs inside a dump of Big Buck HD."""
import argparse, mmap, os, re, struct
from collections import Counter, defaultdict

SIG = b"MDMP"
MEM_LIST, MEM64_LIST = 5, 9
RUN = re.compile(rb"[ -~\r\n\t]{64,}")
LOOSE_WINDOWS = 32


def regions(mm):
    if mm[:4] != SIG:
        print("not a minidump: treating the file as one raw memory image")
        return [(0, 0, len(mm))]
    n_streams, dir_rva = struct.unpack_from("<II", mm, 8)
    out = []
    for i in range(n_streams):
        stype, _size, srva = struct.unpack_from("<III", mm, dir_rva + i * 12)
        if stype == MEM_LIST:
            n = struct.unpack_from("<I", mm, srva)[0]
            for j in range(n):
                start, dsize, drva = struct.unpack_from("<QII", mm, srva + 4 + j * 16)
                out.append((start, drva, dsize))
        elif stype == MEM64_LIST:
            n, base = struct.unpack_from("<QQ", mm, srva)
            off = base
            for j in range(n):
                start, dsize = struct.unpack_from("<QQ", mm, srva + 16 + j * 16)
                out.append((start, off, dsize))
                off += dsize
    return out


def text(data):
    return data.decode("latin-1").replace("\r", "\\r").replace("\n", "\\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("gamedir")
    ap.add_argument("-o", "--out")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    fur = []
    for root, _dirs, files in os.walk(args.gamedir):
        if os.path.basename(root).lower() == "fur":
            fur += [os.path.join(root, f) for f in files]
    fur.sort()
    if not fur:
        raise SystemExit("no prod/conf/fur files under " + args.gamedir)
    by_len = defaultdict(list)
    for p in fur:
        by_len[os.path.getsize(p)].append(p)
    print(f"{len(fur)} encrypted fur files, sizes {sorted(Counter(os.path.getsize(p) for p in fur).items())}")

    fd = open(args.dump, "rb")
    mm = mmap.mmap(fd.fileno(), 0, access=mmap.ACCESS_READ)
    regs = regions(mm)
    print(f"dump: {len(regs)} memory regions, {sum(r[2] for r in regs) / 1e6:.0f} MB")

    exact = defaultdict(list)
    loose = defaultdict(list)
    for vaddr, off, size in regs:
        for m in RUN.finditer(mm, off, off + size):
            s, e = m.span()
            n = e - s
            if n in by_len:
                for p in by_len[n]:
                    exact[p].append((vaddr + (s - off), mm[s:e]))
            for L in by_len:
                if n <= L:
                    continue
                windows, w = 0, s
                while w + L <= e and windows < LOOSE_WINDOWS:
                    for p in by_len[L]:
                        loose[p].append((vaddr + (w - off), mm[w:w + L]))
                    windows += 1
                    w += 16

    print(f"\nfiles with an exact-length plaintext candidate: {sum(1 for p in fur if exact[p])}/{len(fur)}")
    print(f"files with any candidate: {sum(1 for p in fur if exact[p] or loose[p])}/{len(fur)}")

    if args.out:
        os.makedirs(args.out, exist_ok=True)
    for p in fur:
        cands = exact[p] or loose[p]
        if not cands:
            continue
        seen, uniq = set(), []
        for vaddr, data in cands:
            if data not in seen:
                seen.add(data)
                uniq.append((vaddr, data))
        if args.verbose or len(uniq) <= 3:
            print(f"\n{p}  ({os.path.getsize(p)} B, {len(uniq)} candidate(s))")
            for vaddr, data in uniq[:3]:
                print(f"  @0x{vaddr:08x} {text(data)[:160]}")
        if args.out:
            safe = p.replace(os.sep, "_")
            for i, (_vaddr, data) in enumerate(uniq):
                with open(os.path.join(args.out, f"{safe}.{i}.bin"), "wb") as f:
                    f.write(data)

    print("\n=== do the encrypted originals appear in memory? ===")
    for p in fur[:8]:
        c = open(p, "rb").read()
        at = mm.find(c)
        print(f"  {os.path.basename(p)}: {'@0x%08x' % at if at >= 0 else 'absent'}")

    adir = os.path.join(args.gamedir, "hasp")
    if os.path.isdir(adir):
        print("\n=== recorded dongle answers found in memory? ===")
        for f in sorted(os.listdir(adir)):
            b = open(os.path.join(adir, f), "rb").read()
            at = mm.find(b)
            print(f"  {f} ({len(b)} B): {'@0x%08x' % at if at >= 0 else 'absent'}")


if __name__ == "__main__":
    main()
