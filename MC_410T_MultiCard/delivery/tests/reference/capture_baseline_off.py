"""Build the exact approved OLD implementation and capture its OFF output bytes."""
import argparse
import pathlib
import subprocess
import hashlib

p = argparse.ArgumentParser()
p.add_argument('--compiler', required=True)
p.add_argument('--work', required=True)
args = p.parse_args()
root = pathlib.Path(__file__).resolve().parents[4]
work = pathlib.Path(args.work).resolve()
work.mkdir(parents=True, exist_ok=True)
sha = 'a8c15cff203da959bd935120605e69dc32ff7145'
paths = ['include/RingEnhanceConfig.h', 'include/RingSignalEnhancer.h',
         'src/RingSignalEnhancer.cpp', 'src/RingRecon/ring_recon.h', 'src/RingRecon/ring_recon.cpp']
for path in paths:
    data = subprocess.check_output(['git', 'show', f'{sha}:MC_410T_MultiCard/delivery/{path}'], cwd=root)
    (work / pathlib.Path(path).name).write_bytes(data)
source = r'''
#include "RingSignalEnhancer.h"
#include "ring_recon.h"
#include <cmath>
#include <cstdint>
#include <iostream>
int main() {
  std::cout<<"N,flags,delay,fnv1a64\n";
  RingSignalEnhancer old; old.setConfig(RingEnhanceConfig{},250e6);
  for(int n:{2030,4000,5000,7500})for(int flags=0;flags<8;++flags)for(int delay:{1,7,358,371}) {
    std::vector<double> x(n);
    for(int i=0;i<n;++i)x[i]=std::sin(i*.071)+.4*std::cos(i*1.7);
    ringrecon::PreprocessParams pp;
    pp.systemDelay=delay;pp.dbrRemove=flags&1;pp.delayCut=flags&2;pp.signalImpair=flags&4;
    pp.maskLength=300;pp.dbrmaskExtra=13;pp.imValue=.3;
    auto y=ringrecon::preprocessBlock(x,n,1,pp);
    if(!old.apply(y.data(),static_cast<int>(y.size())))return 1;
    std::uint64_t hash=14695981039346656037ULL;
    for(size_t i=0;i<y.size()*sizeof(float);++i) {hash^=reinterpret_cast<const unsigned char*>(y.data())[i];hash*=1099511628211ULL;}
    std::cout<<n<<','<<flags<<','<<delay<<','<<hash<<'\n';
  }
}'''
(work / 'capture.cpp').write_text(source)
subprocess.run([args.compiler, '-std=c++17', '-g', str(work/'capture.cpp'),
    str(work/'RingSignalEnhancer.cpp'), str(work/'ring_recon.cpp'), '-I'+str(work),
    '-o', str(work/'capture.exe')], check=True)
output = subprocess.check_output([str(work/'capture.exe')])
target = pathlib.Path(__file__).resolve().parent / 'baseline_off.csv'
target.write_bytes(output)
print('baseline',sha,'cases',len(output.splitlines())-1,'SHA256',hashlib.sha256(output).hexdigest())
