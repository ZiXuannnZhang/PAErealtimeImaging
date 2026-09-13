import sys,json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parent;sys.path.insert(0,str(ROOT/'tools'))
import pefile,capstone
pe=pefile.PE(str(ROOT/'resources/10-101-1033.bin'));idx=json.loads((ROOT/'binary-index.json').read_text());base=pe.OPTIONAL_HEADER.ImageBase
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);md.skipdata=True
def string_at(va):
 try:
  b=pe.get_data(va-base,500); ss=[]
  for enc in ['utf-16le','ascii']:
   s=b.decode(enc,errors='replace').split(chr(0))[0]
   if len(s)>4 and all(c.isprintable() or c in chr(10)+chr(13)+chr(9) for c in s) and chr(65533) not in s:ss.append(s[:180])
  return repr(ss[0]) if ss else ''
 except:return ''
for arg in sys.argv[1:]:
 args=arg.split(':');start=int(args[0],16); pair=(start,int(args[1],16)) if len(args)>1 else next(((int(a,16),int(b,16)) for a,b in idx['functions'] if int(a,16)==start),None)
 if not pair: print('missing',arg);continue
 out=[]
 for addr,size,mn,op in md.disasm_lite(pe.get_data(start-base,pair[1]-start),start):
  comment='';m=re.search(r'rip ([+-]) (0x[0-9a-f]+)',op)
  if m:
   target=addr+size+(1 if m[1]=='+' else -1)*int(m[2],16)
   comment=hex(target)+' '+idx['imports'].get(hex(target),string_at(target))
  out.append(f'{addr:016x}: {mn:8s} {op:58s} ; {comment}')
 path=ROOT/('fn-'+hex(start)+'.asm');path.write_text(chr(10).join(out),encoding='utf-8');print(path.name,len(out),'instructions',pair)
