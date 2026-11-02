import re, sys
# summ.py LOG FROM TO LABEL: averages of the perf/mem lines whose frame is in [FROM, TO]
log, lo, hi, lab = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
pats = {
    'interval': r'interval avg ([\d.]+) ms', 'max': r'interval avg [\d.]+ ms max ([\d.]+)',
    'busy': r'render busy avg ([\d.]+)', 'replay': r'replay avg ([\d.]+)', 'gpu': r'gpu avg ([\d.]+)',
    'ww_q': r'write-watch frame \d+: per frame, every consumer: ([\d.]+) queries',
    'ww_mib': r'queries over ([\d.]+) MiB', 'ww_ms': r'MiB, ([\d.]+) ms \(', 'frozen': r'frozen ([\d.]+) MiB',
    'thaw_w': r'thaws by a write (\d+)', 'track': r'tracking cpu ([\d.]+)', 'syncs': r'syncs ([\d.]+)',
}
acc = {k: [] for k in pats}
for line in open(log, errors='replace'):
    m = re.search(r'(?:perf|mem: [\w-]+) frame:? ?(\d+)', line) or re.search(r'frame (\d+):', line)
    if not m or not lo <= int(m.group(1)) <= hi: continue
    for k, p in pats.items():
        if k.startswith('ww_') or k in ('frozen', 'thaw_w'):
            if 'write-watch frame' not in line: continue
        x = re.search(p, line)
        if x: acc[k].append(float(x.group(1)))
out = []
for k, v in acc.items():
    if not v: continue
    agg = max(v) if k == 'max' else (sum(v) if k == 'thaw_w' else sum(v) / len(v))
    out.append(f'{k} {agg:.2f}')
iv = acc['interval']
fps = 1000 / (sum(iv) / len(iv)) if iv else 0
print(f'{lab} f{lo}-{hi}: fps {fps:.1f} | ' + ' | '.join(out))
