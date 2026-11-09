import mmap, os, sys, collections
sys.path.insert(0, 'tools')
from analyze_tp_dump import regions, RUN
RUN2 = __import__('re').compile(rb"[ -~\r\n\t]{40,}")
mm = mmap.mmap(os.open('/home/douggle/BudgieLoader.DMP', os.O_RDONLY), 0, access=mmap.ACCESS_READ)
regs = regions(mm)
print("=== what the 208-byte printable runs actually are")
seen = set()
for _v, off, size in regs:
    for m in RUN.finditer(mm, off, off + size):
        s, e = m.span()
        if e - s == 208 and mm[s:e] not in seen:
            seen.add(mm[s:e]); print("  ", mm[s:e][:110].decode('latin-1').replace('\n','\\n'))
print()
print("=== printable runs that end in NUL padding to a 16-byte boundary")
padded = collections.Counter()
samples = []
for _v, off, size in regs:
    for m in RUN2.finditer(mm, off, off + size):
        s, e = m.span(); n = e - s
        pad = (-n) % 16
        if 0 < pad <= 15 and mm[e:e + pad] == b"\0" * pad:
            padded[n + pad] += 1
            if len(samples) < 10:
                samples.append((n, n + pad, mm[s:e][:100]))
print("counts by padded length:", sorted(padded.items())[:14])
for n, L, t in samples:
    print(f"  text={n} padded={L}: {t.decode('latin-1')[:100]!r}")
