exec((__import__('pathlib').Path(__file__).parent/'trace_calls.py').read_text().split('targets=set')[0])
md.skipdata=True
for section in pe.sections:
 if not section.Characteristics & 0x20000000: continue
 for addr,size,mn,op in md.disasm_lite(section.get_data(),base+section.VirtualAddress):
  if mn.startswith('mov') and any(x in op for x in ('xmm10,','xmm11,','xmm12,')) and 0x140139c10<=addr<0x14013b210:
   print(hex(addr),mn,op)
  if mn.startswith('mov') and (('+ 0x80]' in op.split(',')[0]) or ('+ 0xbf0]' in op.split(',')[0])) and not any(x in op.split(',')[0] for x in ('rsp','rbp')):
   print(hex(addr),mn,op)

