# SPDX-License-Identifier: GPL-3.0-or-later
# inject.py IN.hevc RPU.bin OUT.hevc : appends one RPU NAL to each access unit (split at AUDs), in order.
import sys
def nals(b):
    out=[]; i=0; starts=[]
    while True:
        j=b.find(b'\x00\x00\x01', i)
        if j<0: break
        s = j-1 if j>0 and b[j-1]==0 else j
        starts.append((s, j+3)); i=j+3
    for k,(s,p) in enumerate(starts):
        e = starts[k+1][0] if k+1<len(starts) else len(b)
        out.append(b[p:e])
    return out
v=nals(open(sys.argv[1],'rb').read()); r=nals(open(sys.argv[2],'rb').read())
aus=[]
for n in v:
    t=(n[0]>>1)&0x3f
    if t==35 or not aus: aus.append([])
    aus[-1].append(n)
out=bytearray()
for i,au in enumerate(aus):
    for n in au: out+=b'\x00\x00\x00\x01'+n
    if i < len(r): out+=b'\x00\x00\x00\x01'+r[i]
open(sys.argv[3],'wb').write(out)
print(len(aus),'access units,',min(len(aus),len(r)),'RPUs')
