from pathlib import Path
import struct,hashlib,zipfile,shutil
P=Path(wheel_dir)
e=(P/'wheel.elf').read_bytes();loads=[]
ph=struct.unpack_from('<I',e,28)[0];n=struct.unpack_from('<H',e,44)[0]
for i in range(n):
 h=struct.unpack_from('<8I',e,ph+32*i)
 if h[0]==1:loads.append(h)
assert len(loads)==3
align=lambda x:(x+4095)&~4095
image=bytearray(sum(align(h[4]) for h in loads))
for h in loads:
 _,off,va,_,sz,mem,flags,al=h
 assert va+sz<=len(image)
 image[va:va+sz]=e[off:off+sz]
image[16:20]=b'NRO0';struct.pack_into('<I',image,24,len(image))
for i,h in enumerate(loads):struct.pack_into('<II',image,32+8*i,h[2],align(h[4]))
struct.pack_into('<I',image,56,align(max(0,loads[2][5]-align(loads[2][4]))))
image[64:96]=hashlib.sha256(e).digest()
assert image[struct.unpack_from('<I',image,4)[0]:][:4]==b'MOD0'
(P/'gu_craft_wheel.nro').write_bytes(image)
