import hashlib,json,struct,sys
from pathlib import Path
sys.dont_write_bytecode=True
root=Path(__file__).resolve().parent
source=root.parents[2]/'CODEX_REPORTS/reliable-binary-analysis-20260909'
sys.path.insert(0,str(source/'tools'))
import pefile,capstone
binary=source/'resources/10-101-1033.bin'
assert binary.stat().st_size==6132224
assert hashlib.sha256(binary.read_bytes()).hexdigest()=='2b2f4a8b82ff49fc35c2d3d96fe3f999a0dcc403144f39ef520c5af5cea9a6ec'
pe=pefile.PE(str(binary));base=pe.OPTIONAL_HEADER.ImageBase
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
checks={
 0x14001387e:('mov','byte ptr [rbx + 0x58], 1'),
 0x1400138bb:('mov','dword ptr [rbx + 0x80], edi'),
 0x14003aa3d:('mov','r15d, esi'),
 0x14003ad6d:('mov','eax, 0x3e8'),
 0x14003ad72:('cmp','r15d, 4'),
 0x14003ad7c:('cmova','eax, r14d'),
 0x14003ad80:('mov','dword ptr [rbx + 0x80], eax'),
 0x140133dc2:('add','rax, rax'),
 0x140133de1:('cmp','rax, rcx'),
 0x140133e45:('cmp','qword ptr [rax + 0x10], r15'),
 0x14013a226:('lea','rbx, [r12 + 0x3d0]'),
 0x14013a235:('lea','rbx, [r12 + 0x3a8]'),
 0x14013a2fb:('cmp','rcx, rax'),
 0x14014409d:('add','rax, 0x2faf080'),
 0x140150493:('mov','dword ptr [r11], 0xfafafafa'),
 0x14015049a:('mov','byte ptr [r11 + 4], 2'),
 0x140150511:('mov','byte ptr [rcx + 4], 3'),
 0x140150515:('mov','byte ptr [rcx + 0xa], 8'),
 0x140150519:('mov','byte ptr [rcx + 0x39], 1'),
 0x140150573:('mov','byte ptr [rcx + 0x39], 0'),
 0x140150585:('cmp','edx, 0x12'),
 0x140150597:('cmp','edx, 0x3c'),
 0x140136387:('sub','bx, word ptr [r15 + 0x30]'),
 0x14013638f:('cmp','esi, dword ptr [r15 + 0x28]'),
 0x1401363dd:('cmp','r13d, 0x5a0'),
 0x140134e09:('cmp','rax, 0x10'),
 0x140134fe9:('cmp','rdx, 0x1000'),
 0x140135006:('mov','eax, 0x8000000'),
 0x1401350b8:('mov','byte ptr [rsi + 0x408], 0'),
 0x1401343f4:('cmp','qword ptr [rbp + 0x20], 4'),
 0x140134406:('cmp','rbx, 0x1dcd6500'),
 0x140134413:('mov','byte ptr [rdi + 0xbf0], 1'),
 0x140139230:('cmp','rcx, 0x5f5e100'),
 0x140135697:('cmp','rax, 0xee6b280'),
 0x14013569f:('cmp','qword ptr [rsi + 0xbc0], 0x10'),
 0x1401340c6:('cmp','dword ptr [r12], 0x190'),
 0x140131a55:('add','rax, 0x3b9aca00'),
 0x140131b97:('cmp','esi, 3'),
 0x1401431ed:('mov','byte ptr [r14 + 0x408], 1'),
 0x140143ad8:('mov','byte ptr [rbx + 0x408], 0'),
 0x140143aec:('mov','byte ptr [rbx + 0x408], 1'),
 0x14012d398:('mov','qword ptr [rbx + 0x30], 7'),
 0x14012d3a0:('mov','qword ptr [rbx + 0x38], 8'),
 0x14012d3a8:('mov','dword ptr [rbx], 0x3f800000'),
 0x14012b9d9:('movabs','rax, 0xcbf29ce484222325'),
 0x14012b9e6:('movabs','rcx, 0x100000001b3'),
}
results=[]
for va,expected in checks.items():
 ins=next(md.disasm(pe.get_data(va-base,16),va,count=1))
 actual=(ins.mnemonic,ins.op_str)
 assert actual==expected,(hex(va),expected,actual)
 results.append(dict(va=hex(va),rva=hex(va-base),fileOffset=pe.get_offset_from_rva(va-base),bytes=bytes(ins.bytes).hex(),instruction=' '.join(actual)))
constants={hex(va):struct.unpack('<d',pe.get_data(va-base,8))[0] for va in (0x14047e3a0,0x14047e3a8,0x14047e3b0,0x140475cc8)}
output=dict(sourceSha256=hashlib.sha256(binary.read_bytes()).hexdigest(),verifiedInstructionCount=len(results),checks=results,downstreamConstants=constants,
 scope='Exact selected instructions only; not proof of whole-function recovery or production integration')
(root/'recovery-verified.json').write_text(json.dumps(output,indent=2),encoding='utf-8')
print(json.dumps(dict(verified=len(results),constants=constants)))
