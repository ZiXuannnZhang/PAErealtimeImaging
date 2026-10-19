"""Compile exact approved eed98f0 CPU preprocessing; freeze both old switch states."""
from pathlib import Path
import hashlib
import os
import subprocess
root=Path(__file__).resolve().parents[4]
project=root/'MC_410T_MultiCard/delivery'
work=root/'artifacts/filtered-das/baseline/cpu'
work.mkdir(parents=True,exist_ok=True)
sha='eed98f0aea78ea4180e9284ee51a5e71fdf11988'
for name in ['include/RingTimevarLowpass.h','include/RingTimevarLowpassConfig.h','include/RingTimevarPreprocess.h',
             'src/RingTimevarLowpass.cpp','src/RingRecon/ring_recon.h','src/RingRecon/ring_recon.cpp']:
    (work/Path(name).name).write_bytes(subprocess.check_output(['git','show',f'{sha}:MC_410T_MultiCard/delivery/{name}'],cwd=root))
compiler='D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/g++.exe'
env=os.environ.copy();env['PATH']=str(Path(compiler).parent)+';'+env['PATH']
command=[compiler,'-std=c++17','-O2','-DFILTERED_BASELINE_CAPTURE',str(project/'tests/filtered_das_off_test.cpp'),
         str(work/'RingTimevarLowpass.cpp'),str(work/'ring_recon.cpp'),'-I'+str(work),'-o',str(work/'capture.exe')]
subprocess.run(command,check=True,env=env)
data=subprocess.check_output([str(work/'capture.exe')],env=env)
target=project/'tests/reference/filtered_das_baseline_off.csv'
target.write_bytes(data)
print('baseline',sha,'cases',len(data.splitlines())-1,'SHA256',hashlib.sha256(data).hexdigest())
