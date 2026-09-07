# 迁移后验证.ps1
# 用途：新工作区迁移完成后一次性核对：
#   1) Git 仓库/提交号/备份分支/未提交修改
#   2) 新配置的 CMakeCache 不再残留旧绝对路径
#   3) 关键可执行文件存在
#   4) 注册表采样率键（若与旧机同 registry 则期望 4.0 / 2.5e+08）
#
# 用法（在新工作区中执行）：
#   pwsh -ExecutionPolicy Bypass -File .\_migration_pack\迁移后验证.ps1

param(
    [string]$WorkspaceRoot = "",
    [string]$OldRoot = "D:\DSHWorkspace\realtime_imaging_migration",
    [string]$GitExe = ""
)

$ErrorActionPreference = "Continue"
$failures = 0

if (-not $WorkspaceRoot) {
    $WorkspaceRoot = Split-Path -Parent $PSScriptRoot
}
$delivery = Join-Path $WorkspaceRoot "MC_410T_MultiCard\delivery"
$buildDir = Join-Path $delivery "build\mingw_make"
$binDir   = Join-Path $buildDir "bin"

function Check([string]$name, [bool]$ok, [string]$detail = "") {
    if ($ok) { Write-Host ("[PASS] {0} {1}" -f $name, $detail) }
    else { $script:failures++; Write-Host ("[FAIL] {0} {1}" -f $name, $detail) }
}

Write-Host "== 1. Git repository =="
if (-not $GitExe) {
    $portable = Join-Path $WorkspaceRoot "_tools\git\cmd\git.exe"
    $GitExe = if (Test-Path -LiteralPath $portable) { $portable } else { "git" }
}
$isRepo = Test-Path -LiteralPath (Join-Path $WorkspaceRoot ".git")
Check "git repo exists" $isRepo
if ($isRepo) {
    $head = (& $GitExe -C $WorkspaceRoot rev-parse HEAD 2>&1).ToString().Trim()
    Check "HEAD is 8000b76" ($head -eq "8000b763c56a2da38ee7d50877a80126c57e1e5a") "HEAD=$head"

    $branches = (& $GitExe -C $WorkspaceRoot branch --format "%(refname:short)" 2>&1)
    Check "backup/cf-dmas-pcf-20260816 exists" ($branches -contains "backup/cf-dmas-pcf-20260816")
    Check "backup/sysdelay-per-channel-20260817 exists" ($branches -contains "backup/sysdelay-per-channel-20260817")

    Write-Host "`ngit status --short:"
    & $GitExe -C $WorkspaceRoot status --short
    Write-Host "(Expect: 12 M tracked files + old Handoff/HANDOFF D + new names ?? + untracked data folders)"
}

Write-Host "`n== 2. Build cache path check =="
$cache = Join-Path $buildDir "CMakeCache.txt"
Check "new CMakeCache.txt exists" (Test-Path -LiteralPath $cache)
if (Test-Path -LiteralPath $cache) {
    # CMakeCache 内路径统一为 '/', 但 OldRoot 参数可能给 '\'; 两种分隔符都查
    $oldForward = $OldRoot -replace '\\','/'
    $oldBack    = $OldRoot -replace '/','\'
    $staleHits = @(
        Select-String -Path $cache -Pattern $oldForward -SimpleMatch -ErrorAction SilentlyContinue
        Select-String -Path $cache -Pattern $oldBack -SimpleMatch -ErrorAction SilentlyContinue
    ) | Where-Object { $_ }
    Check "no old root path in CMakeCache" ($staleHits.Count -eq 0) ("hits=$($staleHits.Count)")
    $home = (Select-String -Path $cache -Pattern "CMAKE_HOME_DIRECTORY" | Select-Object -First 1).Line
    Write-Host "CMAKE_HOME_DIRECTORY -> $home"
    $expectHome = "CMAKE_HOME_DIRECTORY:INTERNAL=$($delivery -replace '\\','/')"
    Check "CMAKE_HOME_DIRECTORY points to new workspace" ($home -eq $expectHome) "expected: $expectHome"
}

Write-Host "`n== 3. Key artifacts =="
Check "MC410T_Receiver.exe" (Test-Path -LiteralPath (Join-Path $binDir "MC410T_Receiver.exe"))
Check "ImagingSvc.exe"        (Test-Path -LiteralPath (Join-Path $binDir "ImagingSvc.exe"))
Check "ring_svc_selftest.exe" (Test-Path -LiteralPath (Join-Path $binDir "ring_svc_selftest.exe"))
Check "ring_recon_cuda.dll"   (Test-Path -LiteralPath (Join-Path $binDir "ring_recon_cuda.dll"))
Check "cudart64_12.dll"       (Test-Path -LiteralPath (Join-Path $binDir "cudart64_12.dll"))

Write-Host "`n== 4. Registry sampling-rate keys (same-machine migration only) =="
try {
    $s = (Get-ItemProperty -Path "HKCU:\Software\MC410T\MC410T_Receiver\AcquisitionParams" -ErrorAction Stop).SampleIntervalNs
    $d = (Get-ItemProperty -Path "HKCU:\Software\MC410T\MC410T_Receiver\ImagingParams\General" -ErrorAction Stop).DaqHz
    Check "SampleIntervalNs == '4.0'" ($s -eq "4.0") "value=$s"
    Check "DaqHz == '2.5e+08'"        ($d -eq "2.5e+08") "value=$d"
} catch {
    Write-Host "[INFO] Registry keys not found on this machine; program defaults to 250MHz (4.0ns) anyway."
}

Write-Host "`n== Result =="
if ($failures -eq 0) {
    Write-Host "ALL CHECKS PASSED"
    exit 0
} else {
    Write-Host "$failures check(s) failed"
    exit 1
}
