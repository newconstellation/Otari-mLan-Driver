import sys, os, gzip
data = open(sys.argv[1],'rb').read(); out = sys.argv[2]
if data[:2]==b'\x1f\x8b': data = gzip.decompress(data)
i=0
while i < len(data):
    m = data[i:i+6]
    if m == b'070707':
        h=data[i:i+76]; mode=int(h[18:24],8); nl=int(h[59:65],8); fs=int(h[65:76],8)
        name=data[i+76:i+76+nl-1].decode(); i+=76+nl; body=data[i:i+fs]; i+=fs
    elif m in (b'070701',b'070702'):
        h=data[i:i+110]; mode=int(h[14:22],16); fs=int(h[54:62],16); nl=int(h[94:102],16)
        name=data[i+110:i+110+nl-1].decode(); i+=110+nl; i=(i+3)&~3; body=data[i:i+fs]; i+=fs; i=(i+3)&~3
    else: raise SystemExit('bad magic at %d %r'%(i,m))
    if name=='TRAILER!!!': break
    p=os.path.abspath(os.path.join(out,name))
    if not p.startswith(os.path.abspath(out)): continue
    t=mode&0o170000
    if t==0o040000: os.makedirs(p,exist_ok=True)
    elif t==0o100000:
        os.makedirs(os.path.dirname(p),exist_ok=True); open(p,'wb').write(body)
    elif t==0o120000:
        os.makedirs(os.path.dirname(p),exist_ok=True)
        try: os.symlink(body.decode(), p)
        except FileExistsError: pass
