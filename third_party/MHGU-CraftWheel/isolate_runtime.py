"""Reproducible namespace fork of the pinned MIT Forge binary.

Same-length replacements preserve addresses and ARM instructions. SysV symbol
hashes and NSO segment hashes are rebuilt, not bypassed. No game binary needed.
"""
from pathlib import Path
import hashlib, struct, sys, zipfile, json
P=Path(__file__).resolve().parent
import lz4.block

def u32(b,o): return struct.unpack_from('<I',b,o)[0]
def put(b,o,v): struct.pack_into('<I',b,o,v)
def elfhash(s):
 h=0
 for c in s:
  h=(h<<4)+c;g=h&0xf0000000
  if g:h^=g>>24
  h&=~g
 return h

def renamed(s):
 if s.startswith(b'forge_'):return b'wcore_'+s[6:]
 if s.startswith(b'ig'):return b'wg'+s[2:]
 if s.startswith(b'Im'):return b'Wm'+s[2:]
 return s

def table(img):
 mod=u32(img,4);assert img[mod:mod+4]==b'MOD0'
 dyn=mod+u32(img,mod+4);dt={}
 for o in range(dyn,len(img),8):
  tag,val=struct.unpack_from('<II',img,o)
  if not tag:break
  dt[tag]=val
 assert 4 in dt,'Expected a SysV symbol count'
 ht=dt[4];count=u32(img,ht+4);strings=dt[5];syms=dt[6]
 symbols=[]
 for i in range(count):
  o=syms+i*16;no=u32(img,o);end=img.index(0,strings+no)
  symbols.append((i,o,strings+no,bytes(img[strings+no:end]),img[o+12],struct.unpack_from('<H',img,o+14)[0]))
 return dt,symbols

def isolate(img,plugin=False):
 dt,before=table(img);result=bytearray(img)
 assert 0x6ffffef5 not in dt,'Mutating GNU hash modules is not supported'
 # Resource paths and loader callback names embedded outside dynstr as well.
 for a,b in [(b'forge_',b'wcore_'),(b'nativeNX/forge',b'nativeNX/wcore'),
             (b'nativeNX/plugins',b'nativeNX/wheelpl'),(b'[forge]',b'[wcore]'),(b'forge\0',b'wcore\0'),(b'forge-loader.log',b'wcore-loader.log')]:
  assert len(a)==len(b);result=result.replace(a,b)
 for i,o,pos,name,info,section in before:
  new=renamed(name);result[pos:pos+len(name)]=new
  if plugin and section and not new.startswith(b'wcore_on'):
   result[o+13]=(result[o+13]&~3)|2 # internal implementation, not a shared ABI
 # Assert suffix sharing in .dynstr did not accidentally change other names.
 _,after=table(result)
 for old,new in zip(before,after):assert new[3]==renamed(old[3]),(old,new)
 ht=dt[4];nb=u32(result,ht);nc=u32(result,ht+4)
 buckets=[0]*nb;chains=[0]*nc
 for i,o,pos,name,info,section in after[1:]:
  bucket=elfhash(name)%nb
  chains[i]=buckets[bucket];buckets[bucket]=i
 result[ht+8:ht+8+4*(nb+nc)]=struct.pack('<'+'I'*(nb+nc),*(buckets+chains))
 # Verify every symbol can be found through rebuilt lookup chains.
 for i,o,pos,name,info,section in after[1:]:
  current=buckets[elfhash(name)%nb];seen=set()
  while current and after[current][3]!=name:
   assert current not in seen;seen.add(current);current=chains[current]
  assert current and after[current][3]==name
 exports=lambda ss,data:{s[3] for s in ss if s[5] and (s[4]>>4) in (1,2) and s[3] and data[s[1]+13]&3 not in (1,2)}
 old_exports=exports(before,img);new_exports=exports(after,result)
 collisions=old_exports&new_exports
 assert not collisions,sorted(collisions)
 return result,{'symbols':len(after),'original_exports':len(old_exports),
                'isolated_exports':len(new_exports),'overlapping_exports':len(collisions)},before,after

def decode_nso(b):
 assert b[:4]==b'NSO0';segments=[];flags=u32(b,12)
 for i in range(3):
  off,va,size=struct.unpack_from('<III',b,0x10+16*i)
  n=u32(b,0x60+4*i)
  raw=lz4.block.decompress(b[off:off+n],uncompressed_size=size) if flags&(1<<i) else b[off:off+size]
  assert len(raw)==size
  if flags&(1<<(i+3)):assert hashlib.sha256(raw).digest()==b[0xa0+32*i:0xc0+32*i]
  segments.append((va,raw))
 img=bytearray(max(va+len(raw) for va,raw in segments))
 for va,raw in segments:img[va:va+len(raw)]=raw
 return img,segments

def encode_nso(original,img,segments):
 b=bytearray(original[:0x100]);put(b,12,(u32(b,12)&~7)|0x38)
 b[0x40:0x60]=hashlib.sha256(img).digest()
 # Preserve the optional module-name blob before segment zero.
 first=u32(original,0x10)
 b.extend(original[0x100:first])
 for i,(va,raw) in enumerate(segments):
  while len(b)%16:b.append(0)
  modified=bytes(img[va:va+len(raw)])
  put(b,0x10+16*i,len(b));put(b,0x60+4*i,len(raw))
  b[0xa0+32*i:0xc0+32*i]=hashlib.sha256(modified).digest();b.extend(modified)
 return bytes(b)

def build():
 out=P/'isolated';out.mkdir(exist_ok=True)
 original=(P/'hardware_forge/subsdk.nso').read_bytes()
 img,segs=decode_nso(original);new,report,oldsyms,newsyms=isolate(img)
 nso=encode_nso(original,new,segs);decoded,_=decode_nso(nso);assert decoded==new
 (out/'subsdk6').write_bytes(nso)
 nro=(P/'gu_craft_wheel.nro').read_bytes();newnro,nreport,imports,changed=isolate(bytearray(nro),plugin=True)
 newnro[64:96]=bytes(32);newnro[64:96]=hashlib.sha256(newnro).digest()
 (out/'gu_craft_wheel.nro').write_bytes(newnro)
 exports={s[3] for s in newsyms if s[5]}
 for s in changed:
  if s[3].startswith((b'wcore_',b'wg',b'Wm')) and not s[5]:assert s[3] in exports,s[3]
 report['plugin']=nreport;report['runtime_sha256']=hashlib.sha256(nso).hexdigest()
 report['plugin_sha256']=hashlib.sha256(newnro).hexdigest()
 (out/'audit.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
 print(json.dumps(report,indent=2))
 return out

if __name__=='__main__':build()
