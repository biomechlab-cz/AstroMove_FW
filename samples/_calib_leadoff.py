import csv, sys

def chunks(path, w=1000):
    vals = []
    with open(path) as f:
        r = csv.reader(f); next(r, None)
        for row in r:
            if len(row) >= 3 and row[2].lstrip('-').isdigit():
                vals.append(int(row[2]))
    for s0 in range(0, len(vals) - w + 1, w):
        yield vals[s0:s0+w]

def metrics(seg):
    n = len(seg)
    mean = sum(seg) / n
    mad = sum(abs(v - mean) for v in seg) / n            # mean abs deviation (counts)
    diff = sum(abs(seg[i] - seg[i-1]) for i in range(1, n))  # firmware's diff_abs_sum
    p2p = max(seg) - min(seg)
    return mad, diff, p2p

for path in sys.argv[1:]:
    name = path.replace("\\", "/").split("/")[-1]
    mads, diffs, p2ps = [], [], []
    for seg in chunks(path):
        mad, diff, p2p = metrics(seg)
        mads.append(mad); diffs.append(diff); p2ps.append(p2p)
    def rng(a): return f"min={min(a):>10.0f} med={sorted(a)[len(a)//2]:>10.0f} max={max(a):>10.0f}"
    print(f"{name:22} chunks={len(mads)}")
    print(f"    MAD(counts)     {rng(mads)}")
    print(f"    diff_abs_sum    {rng(diffs)}")
    print(f"    peak_to_peak    {rng(p2ps)}")
