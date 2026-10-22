import sys, struct, zlib, os, xml.etree.ElementTree as ET
src, out = sys.argv[1], sys.argv[2]
d = open(src,'rb').read()
magic, hs, ver, tlc, tlu, ck = struct.unpack('>4sHHQQI', d[:28])
toc = zlib.decompress(d[hs:hs+tlc]); heap = hs+tlc
os.makedirs(out, exist_ok=True); open(os.path.join(out,'TOC.xml'),'wb').write(toc)
root = ET.fromstring(toc)
def walk(el, path):
    for f in el.findall('file'):
        name = f.findtext('name'); p = os.path.join(path, name)
        if f.findtext('type') == 'directory':
            os.makedirs(p, exist_ok=True); walk(f, p)
        else:
            da = f.find('data')
            if da is None: open(p,'wb').close(); continue
            off=int(da.findtext('offset')); ln=int(da.findtext('length'))
            enc = da.find('encoding').get('style'); raw = d[heap+off:heap+off+ln]
            if 'gzip' in enc: raw = zlib.decompress(raw)
            elif 'bzip2' in enc: import bz2; raw = bz2.decompress(raw)
            open(p,'wb').write(raw); print(p, len(raw))
walk(root.find('toc'), out)
