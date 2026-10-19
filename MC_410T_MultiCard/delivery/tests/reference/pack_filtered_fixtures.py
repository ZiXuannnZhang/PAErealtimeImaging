"""Lossless Qt qCompress framing (big-endian byte count + zlib), no numeric conversion."""
from pathlib import Path
import hashlib
import json
import struct
import zlib
folder=Path(__file__).resolve().parent/'filtered_das_fixtures'
records=[]
for file in sorted(folder.glob('*.bin')):
    data=file.read_bytes();packed=struct.pack('>I',len(data))+zlib.compress(data,9)
    target=file.with_suffix('.bin.qz');target.write_bytes(packed)
    assert zlib.decompress(packed[4:])==data
    records.append({'name':file.name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),
                    'packed_bytes':len(packed),'packed_sha256':hashlib.sha256(packed).hexdigest()})
    file.unlink()  # Only our generated, verified losslessly packed temporary outputs.
assert len(records)==354
(folder/'hashes.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
print('fixtures',len(records),'raw_bytes',sum(x['bytes'] for x in records),'packed_bytes',sum(x['packed_bytes'] for x in records))
