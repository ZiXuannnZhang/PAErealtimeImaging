import sys,json,hashlib
from pathlib import Path
ROOT=Path(__file__).resolve().parent;sys.path.insert(0,str(ROOT/'tools'))
import pefile,capstone
installer=ROOT.parent.parent/'PAimage_Setup.exe'
binary=ROOT/'resources/10-101-1033.bin'
pe=pefile.PE(str(binary));base=pe.OPTIONAL_HEADER.ImageBase
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
expected={
 0x140138edd:('mov','edx, 2'),
 0x1401385e7:('mov','dword ptr [rsp + 0x38], 0x4000000'),
 0x140138601:('mov','r8d, 0x1002'),
 0x140139119:('mov','dword ptr [rsp + 0x44], 0x3e8'),
 0x1401394d1:('jne','0x140139480'),
 0x140136183:('movzx','edi, byte ptr [rsi + 1]'),
 0x140136387:('sub','bx, word ptr [r15 + 0x30]'),
 0x14013638f:('cmp','esi, dword ptr [r15 + 0x28]'),
 0x140135798:('cmp','byte ptr [rsi + 0xbf0], 0'),
 0x1401357a1:('inc','qword ptr [rsi + 0x750]'),
 0x1401357af:('inc','qword ptr [rsi + 0x780]'),
 0x140132ab4:('add','qword ptr [rsi + 0x778], r12'),
 0x140132ac2:('add','qword ptr [rsi + 0x770], r13'),
}
evidence=[]
for va,exp in expected.items():
 ins=next(md.disasm(pe.get_data(va-base,16),va));actual=(ins.mnemonic,ins.op_str)
 assert actual==exp,(hex(va),actual,exp)
 evidence.append({'va':hex(va),'rva':hex(va-base),'fileOffset':hex(pe.get_offset_from_rva(va-base)),'bytes':ins.bytes.hex(),'instruction':ins.mnemonic+' '+ins.op_str})
resource_list=[]
for p in sorted((ROOT/'resources').glob('*.bin')):
 resource_list.append({'resource':p.name,'size':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()})
result={'installer':{'file':installer.name,'size':installer.stat().st_size,'sha256':hashlib.sha256(installer.read_bytes()).hexdigest()},'main':{'resource':binary.name,'originalFilename':'PAimage.exe','version':'1.5.16.0','size':binary.stat().st_size,'sha256':hashlib.sha256(binary.read_bytes()).hexdigest()},'instructions':evidence,'resources':resource_list,'scope':'Static inspection only. No installer execution, application launch, or hardware experiment.'}
(ROOT/'verified-evidence.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
print('PASS: '+str(len(evidence))+' binary instruction checks; hashes recorded.')
