import sys,bisect,collections,re
# callers.py SCRIPT SYMS PREFIX [DEPTH]: caller chains of samples whose leaf name starts with PREFIX
syms=[(int(a,16),n) for a,_,n in (l.split()[:3] for l in open(sys.argv[2]) if len(l.split())>=3)]
addrs=[a for a,_ in syms]; pre=sys.argv[3]; d=int(sys.argv[4]) if len(sys.argv)>4 else 3
def name(ip,rest):
    if 'ucrtbase' in rest: return 'ucrt:'+hex(ip-0x6fffff9b0000)
    if 0x140000000<=ip<0x160000000:
        i=bisect.bisect_right(addrs,ip)-1; return syms[i][1] if i>=0 else hex(ip)
    m=re.search(r'(\S+) \((\S+)\)',rest)
    return (m.group(1)+'@'+m.group(2).split('/')[-1]) if m else rest.strip()
samples=[];cur=None
for l in open(sys.argv[1]):
    if l.startswith('\t'):
        p=l.split(None,1); cur['st'].append(name(int(p[0],16),p[1] if len(p)>1 else ''))
    elif l.strip():
        cur={'tid':l.split()[0],'st':[]}; samples.append(cur)
hits=[s for s in samples if s['st'] and s['st'][0].startswith(pre)]
print(pre,'samples',len(hits),'of',len(samples),'by tid',collections.Counter(s['tid'] for s in hits).most_common(3))
for k,v in collections.Counter(' <- '.join(s['st'][1:1+d]) for s in hits).most_common(12): print(v,k[:220])
