import bisect, collections, re, sys
# split.py PERF.DATA-REPORT MAP: share of one thread's samples by origin.
# Input 1: lines "pct dso addr_or_sym" (from perf report --sort dso,sym). Input 2: totk.map (ld map, base 0x140000000).
rows = [l.split(None, 2) for l in open(sys.argv[1])]
secs = []
for l in open(sys.argv[2], errors='replace'):
    m = re.match(r'\s*([0-9a-f]{8}) ([0-9a-f]{8})\s+\d+\s+(\S+?):?\(\.text', l)
    if m and int(m.group(2), 16):
        secs.append((0x140000000 + int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
secs.sort(); starts = [s[0] for s in secs]

def origin(obj):
    if '/obj/runtime/' in obj: return 'runtime'
    if '/obj/' in obj: return 'game:' + obj.split('/obj/')[1].split('/')[0]
    return 'libstdc++/crt'

c = collections.Counter(); objs = collections.Counter(); tot = 0
for p, dso, s in rows:
    p = float(p.rstrip('%')); tot += p; s = s.strip()
    if dso == '[JIT]' or dso.endswith('totk.exe'):
        a = int(s.split()[-1], 16) if '0x' in s else None
        i = bisect.bisect_right(starts, a) - 1 if a else -1
        if i >= 0 and a < secs[i][0] + secs[i][1]:
            o = origin(secs[i][2]); c[o] += p; objs[secs[i][2].split('/obj/')[-1]] += p
        else: c['exe:unmapped'] += p
    elif 'kernel' in dso or dso == '[amdgpu]': c['kernel'] += p
    elif dso.startswith(('ntdll', 'win32u', 'winevulkan', 'ucrtbase', 'kernelbase', 'kernel32')): c['wine:' + dso.split('.')[0]] += p
    elif 'vulkan_radeon' in dso: c['radv'] += p
    else: c['other:' + dso] += p
for k, v in c.most_common(): print(f'{100*v/tot:5.1f}% {k}')
print('top objects:', ', '.join(f'{k} {100*v/tot:.1f}%' for k, v in objs.most_common(6)))
