param([string]$QtRoot='D:/Qt/Qt6.8.0')
$ErrorActionPreference='Stop'
$auditRoot=Split-Path $PSScriptRoot -Parent
$qt=Join-Path $QtRoot '6.8.0/mingw_64'
$toolchain=Join-Path $QtRoot 'Tools/mingw1310_64/bin'
$cmake=Join-Path $QtRoot 'Tools/CMake_64/bin/cmake.exe'
$ctest=Join-Path $QtRoot 'Tools/CMake_64/bin/ctest.exe'
$ninja=Join-Path $QtRoot 'Tools/Ninja/ninja.exe'
$env:PATH=(Join-Path $qt 'bin')+';'+$toolchain+';'+$env:PATH
# Sequential: both suites use fixed loopback UDP ports.
foreach($branch in @('main','latest')) {
    $source=Join-Path $auditRoot ($branch+'/MC_410T_MultiCard/delivery/tests')
    $build=Join-Path $auditRoot ('build-'+$branch)
    & $cmake -S $source -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_CXX_COMPILER=$toolchain/g++.exe" "-DCMAKE_PREFIX_PATH=$qt" -DCMAKE_BUILD_TYPE=Debug
    if($LASTEXITCODE -ne 0) {throw 'Configure failed'}
    & $cmake --build $build -j 6
    if($LASTEXITCODE -ne 0) {throw 'Build failed'}
    & (Join-Path $qt 'bin/windeployqt.exe') --no-translations --no-system-d3d-compiler --no-opengl-sw (Join-Path $build 'diagnostic_dialog_test.exe')
    if($LASTEXITCODE -ne 0) {throw 'Qt deployment failed'}
    Copy-Item (Join-Path $qt 'bin/Qt6Concurrent.dll') $build -Force
    Copy-Item (Join-Path $qt 'plugins/platforms/qoffscreen.dll') (Join-Path $build 'platforms') -Force
    & $ctest --test-dir $build --output-on-failure --timeout 60 -j 1
    if($LASTEXITCODE -ne 0) {throw 'CTest failed; inspect environment and test output'}
}
