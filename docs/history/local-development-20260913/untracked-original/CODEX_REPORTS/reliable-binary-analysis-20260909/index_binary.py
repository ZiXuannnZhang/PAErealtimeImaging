import sys,json,bisect,re
from pathlib import Path
ROOT=Path(__file__).resolve().parent;sys.path.insert(0,str(ROOT/'tools'))
import pefile,capstone
binary=ROOT/'resources/10-101-1033.bin';pe=pefile.PE(str(binary));base=pe.OPTIONAL_HEADER.ImageBase
ws=pefile.PE('C:/Windows/System32/ws2_32.dll');ords={s.ordinal:s.name.decode() for s in ws.DIRECTORY_ENTRY_EXPORT.symbols if s.name}
imports={}
for dll in pe.DIRECTORY_ENTRY_IMPORT:
 for item in dll.imports:
  name=item.name.decode() if item.name else ords.get(item.ordinal,str(item.ordinal)) if dll.dll.lower()==b'ws2_32.dll' else str(item.ordinal)
  imports[item.address]=dll.dll.decode()+'!'+name
funcs=[(base+e.struct.BeginAddress,base+e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION];starts=[a for a,b in funcs]
def owner(addr):
 i=bisect.bisect_right(starts,addr)-1
 return hex(starts[i]) if i>=0 and addr<funcs[i][1] else None
refs={}; calls={}; rip=re.compile(r'rip ([+-]) (0x[0-9a-f]+)')
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
md.skipdata=True
for sec in pe.sections:
 if sec.Name.startswith(b'.text'):
  for addr,size,mn,op in md.disasm_lite(sec.get_data(),base+sec.VirtualAddress):
   m=rip.search(op)
   if m:
    target=addr+size+(1 if m[1]=='+' else -1)*int(m[2],16)
    refs.setdefault(hex(target),[]).append({'at':hex(addr),'function':owner(addr),'instruction':mn+' '+op})
   if mn=='call' and op.startswith('0x'):
    calls.setdefault(op,[]).append({'at':hex(addr),'function':owner(addr)})
result={'base':hex(base),'imports':{hex(k):v for k,v in imports.items()},'functions':[(hex(a),hex(b)) for a,b in funcs],'references':refs,'calls':calls}
(ROOT/'binary-index.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
out=[]
for addr,name in imports.items():
 if 'WS2_32' in name.upper() or any(k in name for k in ['ThreadPriority','Affinity','CreateThread','Sleep','WaitForMultiple']):
  out.append(name+' '+hex(addr)+' '+json.dumps(refs.get(hex(addr),[])))
for offset in [0x47c420,0x47c470,0x46b9d8,0x46ba28,0x47c970,0x47cd20]:
 va=base+pe.get_rva_from_offset(offset);out.append('STRING '+hex(offset)+' '+hex(va)+' '+json.dumps(refs.get(hex(va),[])))
(ROOT/'network-xrefs.txt').write_text(chr(10).join(out),encoding='utf-8');print(chr(10).join(out))
