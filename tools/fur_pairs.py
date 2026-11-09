#!/usr/bin/env python3
"""Pair encrypted fur configs with the plaintexts found in a dump, then analyse the cipher."""
import mmap, os, re, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze_tp_dump import regions, RUN

GAME, DUMP = sys.argv[1], sys.argv[2]

fur = []
for root, _d, fs in os.walk(GAME):
    if os.path.basename(root).lower() == "fur":
        fur += [os.path.join(root, f) for f in fs]
fur.sort()
by_len = collections.defaultdict(list)
for p in fur:
    by_len[os.path.getsize(p)].append(p)

fd = open(DUMP, "rb")
mm = mmap.mmap(fd.fileno(), 0, access=mmap.ACCESS_READ)
runs = collections.defaultdict(set)          # length -> {bytes}
for _v, off, size in regions(mm):
    for m in RUN.finditer(mm, off, off + size):
        s, e = m.span()
        if e - s in by_len:
            runs[e - s].add(mm[s:e])
print("unique printable runs per length:", {k: len(v) for k, v in sorted(runs.items())})

paired, ambiguous = [], []
for L, files in by_len.items():
    cands = [(b, b.decode("latin-1").lower()) for b in runs[L]]
    for p in files:
        stem = os.path.basename(p)[:-4].lower()
        token = stem.split("_")[0]
        scored = []
        for b, t in cands:
            score = 3 if stem in t else (1 if token and token in t else 0)
            if score:
                scored.append((score, b))
        best = max((s for s, _ in scored), default=0)
        top = [b for s, b in scored if s == best]
        if best and len(top) == 1:
            paired.append((p, top[0]))
        elif best:
            ambiguous.append((p, len(top)))
        else:
            ambiguous.append((p, 0))

print(f"paired by name: {len(paired)}/{len(fur)}   unresolved: {len(ambiguous)}")
if ambiguous:
    print("  examples:", [(os.path.basename(p), n) for p, n in ambiguous[:5]])

print("\n=== cipher analysis on paired files ===")
periodic = collections.Counter()
keys = collections.Counter()
samples = 0
for p, plain in paired:
    c = open(p, "rb").read()
    x = bytes(a ^ b for a, b in zip(c, plain))
    if all(x[i] == x[i + 16] for i in range(len(x) - 16)):
        periodic[len(x)] += 1
        keys[x[:16]] += 1
    if samples < 2:
        print(f"\n{p}")
        print("  plaintext:", plain.decode("latin-1")[:200].replace("\r", "\\r").replace("\n", "\\n"))
        print("  cipher^plain head:", x[:24].hex())
        samples += 1
print(f"\n16-byte-periodic XOR keystream: {sum(periodic.values())}/{len(paired)} paired files")
if keys:
    print("distinct 16-byte keystreams:", len(keys))
    for k, n in keys.most_common(3):
        print(f"  {k.hex()}  x{n}")
