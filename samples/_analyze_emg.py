import csv, math, sys

path = sys.argv[1]
maxn = int(sys.argv[2]) if len(sys.argv) > 2 else 80000
ch1 = []
with open(path) as f:
    r = csv.reader(f); next(r, None)
    for i, row in enumerate(r):
        if i >= maxn: break
        if len(row) >= 3 and row[2].lstrip('-').isdigit():
            ch1.append(int(row[2]))
n = len(ch1); fs = 1000.0
uv = 2420.0 * 1000.0 / ((1 << 23) * 4)          # µV per count
mean = sum(ch1) / n
x = [c - mean for c in ch1]
std = (sum(v*v for v in x) / n) ** 0.5
rng = max(ch1) - min(ch1)

def amp_at(f):                                   # peak amplitude of sinusoid at f
    w = 2*math.pi*f/fs; s = c = 0.0
    for i in range(n): s += x[i]*math.sin(w*i); c += x[i]*math.cos(w*i)
    return 2.0*math.hypot(s, c)/n
h50 = amp_at(50)

# per-1s window RMS, each window mean-removed (ignores slow drift) -> "activity"
w = 1000; wr = []
for s0 in range(0, n - w + 1, w):
    seg = x[s0:s0+w]; m = sum(seg)/w
    wr.append(((sum((v-m)**2 for v in seg)/w) ** 0.5))
wr_s = sorted(wr)
med = wr_s[len(wr_s)//2]; mx = wr_s[-1]
active = sum(1 for v in wr if v > 3*med)          # windows >3x median = bursts

name = path.replace("\\", "/").split("/")[-1]
print(f"{name:24} n={n:6} | 50Hz={h50*uv:7.0f}uV  std={std*uv:7.0f}uV  pk-pk={rng*uv:7.0f}uV | "
      f"per-s RMS med={med*uv:6.0f}uV max={mx*uv:7.0f}uV  active-windows={active}")
