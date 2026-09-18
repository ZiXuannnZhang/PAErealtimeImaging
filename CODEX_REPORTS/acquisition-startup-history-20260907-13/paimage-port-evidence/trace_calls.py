import sys, json, bisect
sys.dont_write_bytecode=True
from pathlib import Path
root=Path(__file__).resolve().parent
source=root.parents[2]/'CODEX_REPORTS/reliable-binary-analysis-20260909'
sys.path.insert(0,str(source/'tools'))
import pefile, capstone
pe=pefile.PE(str(source/'resources/10-101-1033.bin'))
base=pe.OPTIONAL_HEADER.ImageBase
idx=json.loads((root/'binary-index.json').read_text())
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
targets=set(int(x,16) for x in sys.argv[1:])
out=[]
for a,b in idx['functions']:
 a,b=int(a,16),int(b,16)
 for addr,size,mn,op in md.disasm_lite(pe.get_data(a-base,b-a),a):
  if mn in ('call','jmp') and op.startswith('0x') and int(op,16) in targets:
   out.append(dict(function=hex(a),at=hex(addr),instruction=mn+' '+op))
print(json.dumps(out,indent=2))
