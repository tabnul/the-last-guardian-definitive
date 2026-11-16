"""Top functions of a leaf profile (/profile?thread=<name>&seconds=8&hz=1000&top=200&leaf=1 saved as JSON): python profile_leaf.py <file.json> [top]."""
import json, subprocess, bisect, re, collections, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dev_paths  # noqa: E402
d=json.load(open(sys.argv[1],encoding='utf-8-sig')); top=int(sys.argv[2]) if len(sys.argv)>2 else 35
sym=dev_paths.msys2_bin('llvm-symbolizer.exe'); exe=str(dev_paths.OUT / 'gpu' / 'tlg-probe.exe')
od=dev_paths.msys2_bin('llvm-objdump.exe')
def exports(path):
    out=subprocess.run([od,'-p',path],capture_output=True,text=True).stdout; ex=[]; on=False
    for l in out.split('\n'):
        if 'Export Table' in l: on=True
        m=re.match(r'\s+\d+\s+(0x)?([0-9a-fA-F]+)\s+(\S+)$',l)
        if on and m: ex.append((int(m.group(2),16),m.group(3)))
    ex.sort(); return ex
rows=d['top_self']; dl={}
for m in set(r['module'] for r in rows):
    p=os.path.join(r'C:\Windows\System32',m)
    if os.path.exists(p) and m!='tlg-probe.exe' and m!='nvoglv64.dll': dl[m]=exports(p)
ex=[r for r in rows if r['module'].lower().startswith('tlg-probe')]
out=subprocess.run([sym,'--obj='+exe,'--functions=linkage','--demangle','--no-inlines']+['0x%x'%(0x140000000+int(r['rva'],16)) for r in ex],capture_output=True,text=True).stdout.strip().split('\n\n')
nm={r['rva']:b.strip().split('\n')[0].replace('Core::','').replace('(anonymous namespace)::','').split('(')[0][:60] for r,b in zip(ex,out)}
B=collections.Counter()
for r in rows:
    m=r['module']
    B['ours' if m.lower().startswith('tlg-probe') else 'engine' if m=='unknown' else 'driver' if m=='nvoglv64.dll' else 'system']+=r['self_pct']
print(d['thread'],'samples',d['samples'],{k:round(v,1) for k,v in B.items()})
for r in rows[:top]:
    m=r['module']
    if m.lower().startswith('tlg-probe'): n=nm[r['rva']]
    elif m in dl:
        rva=int(r['rva'],16); e=dl[m]; i=bisect.bisect_right([a for a,_ in e],rva)-1; n='%s!%s'%(m,e[i][1] if i>=0 else '?')
    else: n='%s+%s'%(m,r['rva'])
    print('%5.2f  %s'%(r['self_pct'],n))
