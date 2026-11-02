import sys,bisect,collections
# gfault.py SCRIPT SYMS RENDER_TID: page-fault / clear_page / munmap shares on non-render threads that run game code
syms=[(int(a,16),n) for a,_,n in (l.split()[:3] for l in open(sys.argv[2]) if len(l.split())>=3)]
addrs=[a for a,_ in syms]; rt=sys.argv[3]
def nm(ip,rest):
    if 0x140001000<=ip<0x14e813190: return 'exe:'+syms[bisect.bisect_right(addrs,ip)-1][1]
    r=rest.strip(); return r.split(' (')[0]+'@'+r.split('(')[-1].rstrip(')').split('/')[-1]
th=collections.defaultdict(lambda: collections.Counter()); fr=collections.Counter()
cur=None;tid=None
def done():
    if not cur: return
    k=' '.join(r for _,r in cur if 'kallsyms' in r); c=th[tid]; c['n']+=1
    if 'kallsyms' in cur[0][1]: c['kernel']+=1
    if 'asm_exc_page_fault' in k:
        c['fault']+=1; us=[nm(i,r) for i,r in cur if 'kallsyms' not in r]; fr[' <- '.join(us[:2])]+=1
    if 'clear_page_erms' in cur[0][1]: c['clear']+=1
    if '__x64_sys_munmap' in k: c['munmap']+=1
    if any(0x140001000<=i<0x14e813190 and nm(i,'').startswith('exe:f_') for i,_ in cur[:1]): c['game']+=1
for l in open(sys.argv[1]):
    if l.startswith('\t'):
        p=l.split(None,1); cur.append((int(p[0],16),p[1] if len(p)>1 else ''))
    elif l.strip():
        done(); tid=l.split()[0]; cur=[]
done()
G=[t for t,c in th.items() if t!=rt and c['game']>50]
tot=collections.Counter()
for t in G: tot.update(th[t])
n=tot['n']
print(f"game threads {len(G)}: samples {n}, kernel {100*tot['kernel']/n:.1f}%, page-fault {100*tot['fault']/n:.1f}%, clear_page_erms {100*tot['clear']/n:.1f}%, munmap {100*tot['munmap']/n:.1f}% ({tot['munmap']} samples), guest code {100*tot['game']/n:.1f}%")
print('top faulting user frames:', '; '.join(f'{k} {v}' for k,v in fr.most_common(5)))
