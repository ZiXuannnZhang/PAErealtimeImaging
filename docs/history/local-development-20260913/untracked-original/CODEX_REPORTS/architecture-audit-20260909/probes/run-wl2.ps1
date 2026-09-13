param([string]$Compiler='D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/g++.exe')
$ErrorActionPreference='Stop'
$auditRoot=Split-Path $PSScriptRoot -Parent
foreach($branch in @('main','latest')) {
    $delivery=Join-Path $auditRoot ($branch+'/MC_410T_MultiCard/delivery')
    $svc=[IO.File]::ReadAllText((Join-Path $delivery 'src/ImagingSvc/ImagingSvc.cpp'))
    $begin=$svc.IndexOf('if (m_ringConfig.shiftWL2 && m_ringBlockIndex > 0')
    $end=$svc.IndexOf('for (int w = 0; w < 2; ++w)', $begin)
    $fragment=$svc.Substring($begin,$end-$begin)
    $template=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'wl2_template.cpp'))
    $generated=Join-Path $PSScriptRoot ($branch+'-wl2-extracted.cpp')
    [IO.File]::WriteAllText($generated,$template.Replace('// SERVICE_FRAGMENT',$fragment))
    $binary=Join-Path $PSScriptRoot ($branch+'-wl2-repro.exe')
    & $Compiler -std=c++17 -O0 -static -I (Join-Path $delivery 'src/RingRecon') $generated (Join-Path $delivery 'src/RingRecon/ring_recon.cpp') -o $binary
    if($LASTEXITCODE -ne 0) {throw 'WL2 probe compilation failed'}
    & $binary | Tee-Object -FilePath (Join-Path $auditRoot ('probe-wl2-'+$branch+'.log'))
    if($LASTEXITCODE -ne 0) {throw 'Unexpected WL2 probe result'}
}
