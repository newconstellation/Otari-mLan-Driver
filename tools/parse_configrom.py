#!/usr/bin/env python3
"""Decode an IEEE 1212 config ROM hex dump (as printed by ioreg "FireWire Device ROM")."""
import sys
KEYS={0x01:'Textual descriptor',0x03:'Vendor ID',0x04:'Hardware version',0x0C:'Node capabilities',
      0x0D:'EUI-64',0x11:'Unit directory',0x12:'Unit spec ID',0x13:'Unit SW version',0x17:'Model ID',
      0x14:'Logical unit number',0x15:'Unit location',0x38:'Unit dependent'}
def run(h):
    q=[int(h[i:i+8],16) for i in range(0,len(h)-7,8)]
    bib=q[0]>>24; print(f'Bus info block: {bib} quadlets, bus name {bytes.fromhex(h[8:16])!r}')
    print(f'  bus options 0x{q[2]:08x}  GUID 0x{q[3]:08x}{q[4]:08x}')
    def text(i):
        n=q[i]>>16; b=b''.join(x.to_bytes(4,'big') for x in q[i+3:i+1+n])
        return b.rstrip(b'\0').decode('latin1')
    def dirwalk(i,ind):
        if i>=len(q): print(f'{ind}(not in dump: macOS cached only part of the ROM)'); return
        n=q[i]>>16
        for k in range(1,n+1):
            e=q[i+k]; t=e>>30; key=(e>>24)&0x3f; v=e&0xffffff
            name=KEYS.get(key,f'key 0x{key:02x}')
            if t==0: print(f'{ind}{name}: 0x{v:06x}')
            elif t==1: print(f'{ind}{name} (offset): 0x{v:06x}')
            elif t==2:
                j=i+k+v
                if key==0x0D: print(f'{ind}{name}: 0x{q[j+1]:08x}{q[j+2]:08x}')
                elif j<len(q) and key==0x01: print(f'{ind}{name} (leaf): "{text(j)}"')
                elif j<len(q): print(f'{ind}{name} (leaf): ' + ' '.join(f'{x:08x}' for x in q[j+1:j+1+(q[j]>>16)]))
                else: print(f'{ind}{name} (leaf): (not in dump)')
            else: print(f'{ind}{name}:'); dirwalk(i+k+v,ind+'    ')
    print('Root directory:'); dirwalk(1+bib,'  ')
run(''.join(sys.argv[1:]) if len(sys.argv)>1 else sys.stdin.read().strip())
