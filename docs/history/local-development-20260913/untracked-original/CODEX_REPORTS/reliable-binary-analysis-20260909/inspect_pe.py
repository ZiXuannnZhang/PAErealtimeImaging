import struct,sys,json,re,hashlib
from pathlib import Path
path=Path(sys.argv[1]); data=path.read_bytes()
u16=lambda o:struct.unpack_from('<H',data,o)[0]
u32=lambda o:struct.unpack_from('<I',data,o)[0]
pe=u32(60); n=u16(pe+6); opt=pe+24; sec=opt+u16(pe+20)
sections=[]
for i in range(n):
 o=sec+i*40; name=data[o:o+8].rstrip(bytes([0])).decode(); vs,va,sz,off=struct.unpack_from('<IIII',data,o+8);sections.append((name,va,vs,off,sz))
def rva(addr):
 for name,va,vs,off,sz in sections:
  if va<=addr<va+max(vs,sz):return off+addr-va
 raise ValueError(hex(addr))
dd=opt+(112 if u16(opt)==523 else 96)
print('file',str(path),'sha256',hashlib.sha256(data).hexdigest(),'sections',sections)
print('CLR directory',struct.unpack_from('<II',data,dd+14*8))
rr,rs=struct.unpack_from('<II',data,dd+2*8)
out=Path(sys.argv[2]) if len(sys.argv)>2 else None
if out:out.mkdir(parents=True,exist_ok=True)
if rr:
 base=rva(rr)
 def walk(rel,parts):
  o=base+rel; cnt=u16(o+12)+u16(o+14)
  for i in range(cnt):
   a,b=struct.unpack_from('<II',data,o+16+i*8)
   if a&0x80000000:
    so=base+(a&0x7fffffff); name=data[so+2:so+2+u16(so)*2].decode('utf-16le')
   else:name=str(a)
   pp=parts+[name]
   if b&0x80000000:walk(b&0x7fffffff,pp)
   else:
    addr,size,cp,_=struct.unpack_from('<IIII',data,base+b);start=rva(addr);blob=data[start:start+size]
    print('resource',pp,'size',size,'offset',start,'head',repr(blob[:24]))
    if out:
     fn='-'.join(re.sub('[^a-zA-Z0-9_.-]','_',v) for v in pp)+'.bin';(out/fn).write_bytes(blob)
 walk(0,[])
