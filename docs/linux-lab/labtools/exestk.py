import sys,re,struct,bisect,collections
# exestk.py DUMP SYMS: for samples whose user IP is in the exe .text, the leaf function and the first 2 other exe functions
# found as return addresses on the raw user stack (approximate: stale stack slots can appear)
syms=[(int(a,16),n) for a,_,n in (l.split()[:3] for l in open(sys.argv[2]) if len(l.split())>=3)]
addrs=[a for a,_ in syms]
def sym(a): i=bisect.bisect_right(addrs,a)-1; return syms[i][1]
T0,T1=0x140001000,0x14e813190
leaf=collections.Counter(); calls=collections.defaultdict(collections.Counter); n=0
ip=None; off=None; raw=bytearray(); inraw=False
def flush():
    global n
    if ip is None: return
    n+=1
    if not (T0<=ip<T1) or off is None: return
    f=sym(ip); leaf[f]+=1; st=raw[off:off+4096]; ch=[]
    for i in range(0,len(st)-7,8):
        v=struct.unpack_from('<Q',st,i)[0]
        if T0<=v<T1:
            s=sym(v)
            if s!=f and s not in ch: ch.append(s)
        if len(ch)==2: break
    calls[f][' <- '.join(ch)]+=1
for l in open(sys.argv[1],errors='replace'):
    if 'PERF_RECORD_SAMPLE' in l and 'event:' not in l:
        flush(); ip=None; off=None; raw=bytearray(); inraw=False
    m=re.match(r'\.\.\.\. IP\s+0x([0-9a-f]+)',l)
    if m: ip=int(m.group(1),16)
    m=re.search(r'ustack: size \d+, offset 0x([0-9a-f]+)',l)
    if m: off=int(m.group(1),16)+8
    if 'raw event:' in l: inraw=True; raw=bytearray(); continue
    m=re.match(r'\.\s+([0-9a-f]{4}):\s+((?:[0-9a-f]{2} ){1,16})',l)
    if inraw and m: raw+=bytes.fromhex(m.group(2).replace(' ',''))
flush()
for f,c in leaf.most_common(25):
    top='; '.join(f'{k} {100*v/c:.0f}%' for k,v in calls[f].most_common(2))
    print(f'{100*c/n:5.2f}% {f:26s} <- {top}'[:240])
