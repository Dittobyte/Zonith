import sys,re,struct,bisect,collections
# ustk.py DUMP SYMS: for user samples whose IP is in ucrtbase, first exe return addresses on the raw stack
syms=[(int(a,16),n) for a,_,n in (l.split()[:3] for l in open(sys.argv[2]) if len(l.split())>=3)]
addrs=[a for a,_ in syms]
def sym(a): i=bisect.bisect_right(addrs,a)-1; return syms[i][1]
UB,UE=0x6fffff9b0000,0x6fffffa40000
res=collections.Counter(); tot=collections.Counter()
ip=None; off=None; raw=bytearray(); inraw=False
def flush():
    if ip is None or off is None or not (UB<=ip<UE) or len(raw)<off+64: return
    o=ip-UB; f='memcpy-bulk64' if 0x171c6<=o<0x171fe else ('memcpy-other' if 0x17160<=o<0x17300 else ('memset' if 0x5e520<=o<0x5e700 else hex(o&~0xff)))
    tot[f]+=1
    st=raw[off:off+4096]; chain=[]
    for i in range(0,len(st)-7,8):
        v=struct.unpack_from('<Q',st,i)[0]
        if 0x140001000<=v<0x14e813190: chain.append(sym(v))
        if len(chain)==3: break
    res[(f,' <- '.join(chain))]+=1
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
print('ucrt samples by function:',tot.most_common(6))
for (f,c),n in res.most_common(15): print(n,f,'<-',c[:200])
