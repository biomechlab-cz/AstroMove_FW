import csv, sys

LO = 1_000_000       # QUALITY_LEADOFF_DIFF_SUM_COUNTS    (quiet float)
HI = 12_000_000      # QUALITY_LEADOFF_HI_DIFF_SUM_COUNTS (wild float)
FS = 8388608         # 2^23 full scale

def chunks(path, w=1000):
    vals = []
    with open(path) as f:
        r = csv.reader(f); next(r, None)
        for row in r:
            if len(row) >= 3 and row[2].lstrip('-').isdigit():
                vals.append(int(row[2]))
    for s0 in range(0, len(vals) - w + 1, w):
        yield vals[s0:s0+w]

for path in sys.argv[1:]:
    name = path.replace("\\", "/").split("/")[-1]
    print(f"\n=== {name} ===  sec : diff_abs_sum  peak_to_peak  max_abs(%FS)  -> verdict")
    per_batch = {}
    for i, seg in enumerate(chunks(path)):
        diff = sum(abs(seg[k] - seg[k-1]) for k in range(1, len(seg)))
        p2p = max(seg) - min(seg)
        mabs = max(abs(v) for v in seg)
        if diff < LO:
            verd = "LEADOFF(quiet)"
        elif diff > HI:
            verd = "LEADOFF(wild)"
        else:
            verd = ""
        per_batch.setdefault(i // 10, 0)
        if verd.startswith("LEADOFF"):
            per_batch[i // 10] += 1
        print(f"  {i:3d} : {diff:12,d}  {p2p:11,d}  {100*mabs/FS:6.1f}%   {verd}")
    pred = ",".join(str(per_batch[b]) for b in sorted(per_batch))
    print(f"  predicted ch1_leadoff_chunks per batch: {pred}")
