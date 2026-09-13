param([string]$Compiler='D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/g++.exe')
$ErrorActionPreference='Stop'
$auditRoot=Split-Path $PSScriptRoot -Parent
foreach($branch in @('main','latest')) {
    $delivery=Join-Path $auditRoot ($branch+'/MC_410T_MultiCard/delivery')
    $saver=[IO.File]::ReadAllText((Join-Path $delivery 'src/FileSaver.cpp'))
    $begin=$saver.IndexOf('uint16_t FileSaver::float32ToFloat16(')
    $end=$saver.IndexOf('void FileSaver::convertBatch(', $begin)
    $function=$saver.Substring($begin,$end-$begin).Replace('FileSaver::','')
    $extracted=Join-Path $PSScriptRoot ($branch+'-half-extracted.cpp')
    [IO.File]::WriteAllText($extracted, '#include <cstdint>'+[Environment]::NewLine+'#include <cstring>'+[Environment]::NewLine+$function)
    $binary=Join-Path $PSScriptRoot ($branch+'-core-repro.exe')
    & $Compiler -std=c++17 -O0 -static -I (Join-Path $delivery 'include') (Join-Path $PSScriptRoot 'core_repro.cpp') $extracted (Join-Path $delivery 'src/RingBlockAssembler.cpp') -o $binary
    if($LASTEXITCODE -ne 0) { throw 'Probe compilation failed' }
    & $binary | Tee-Object -FilePath (Join-Path $auditRoot ('probe-'+$branch+'.log'))
    if($LASTEXITCODE -ne 0) { throw 'Unexpected probe result' }
}
