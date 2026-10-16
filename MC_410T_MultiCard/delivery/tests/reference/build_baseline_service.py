"""Build old exact-SHA service only, against the SAME preserved runtime dependencies."""
import argparse
import pathlib
import subprocess
p=argparse.ArgumentParser();p.add_argument('--work',required=True);a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[4];work=pathlib.Path(a.work).resolve();work.mkdir(parents=True,exist_ok=True)
sha='a8c15cff203da959bd935120605e69dc32ff7145'
for path in ['include/RingEnhanceConfig.h','include/RingSignalEnhancer.h','src/RingSignalEnhancer.cpp',
    'src/ImagingSvc/ImagingSvc.h','src/ImagingSvc/ImagingSvc.cpp','src/ImagingSvc/main.cpp',
    'src/ImagingSvc/ProcessScheduling.h','src/ImagingSvc/ProcessScheduling.cpp',
    'src/RingRecon/ring_recon.h','src/RingRecon/ring_recon.cpp']:
    (work/pathlib.Path(path).name).write_bytes(subprocess.check_output(['git','show',f'{sha}:MC_410T_MultiCard/delivery/{path}'],cwd=root))
delivery=(root/'MC_410T_MultiCard/delivery').as_posix();runtime=(root/'_migration_pack/prebuilt_cuda/bin').as_posix()
cmake=f'''cmake_minimum_required(VERSION 3.21)
project(old_service LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core)
add_executable(ImagingSvc main.cpp ImagingSvc.cpp ImagingSvc.h ProcessScheduling.cpp RingSignalEnhancer.cpp ring_recon.cpp)
target_include_directories(ImagingSvc PRIVATE . "{delivery}/include" "{delivery}/src/RingRecon" "{delivery}/libs/imaging" "{delivery}/third_party/zeromq/include")
target_link_libraries(ImagingSvc PRIVATE Qt6::Core ws2_32 "{runtime}/libring_recon_cuda.dll.a" "{delivery}/libs/imaging/pa_recon_core.lib" "{delivery}/third_party/zeromq/lib/libzmq.dll.a")
'''
(work/'CMakeLists.txt').write_text(cmake)
subprocess.run(['D:/Qt/Qt6.8.0/Tools/CMake_64/bin/cmake.exe','-S',str(work),'-B',str(work/'build'),'-G','Ninja',
    '-DCMAKE_BUILD_TYPE=Debug','-DCMAKE_CXX_COMPILER=D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/g++.exe',
    '-DQt6_DIR=D:/Qt/Qt6.8.0/6.8.0/mingw_64/lib/cmake/Qt6'],check=True)
subprocess.run(['D:/Qt/Qt6.8.0/Tools/CMake_64/bin/cmake.exe','--build',str(work/'build'),'-j','4'],check=True)
print('EXACT BASELINE',sha,'production service sources unmodified')
