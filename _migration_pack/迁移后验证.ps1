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
    [string]$GitExe = "",
    [string]$ExpectedBase = "8000b763c56a2da38ee7d50877a80126c57e1e5a"
)

$ErrorActionPreference = "Continue"
$failures = 0

if (-not $WorkspaceRoot) {
    $WorkspaceRoot = Split-Path -Parent $PSScriptRoot
}
$WorkspaceRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
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
    $gitArgs = @('-c', "safe.directory=$WorkspaceRoot", '--no-optional-locks', '-C', $WorkspaceRoot)
    $head = (& $GitExe @gitArgs rev-parse HEAD 2>&1).ToString().Trim()
    & $GitExe @gitArgs merge-base --is-ancestor $ExpectedBase HEAD
    Check "HEAD contains migration baseline" ($LASTEXITCODE -eq 0) "HEAD=$head; baseline=$ExpectedBase"

    $branches = (& $GitExe @gitArgs branch --format "%(refname:short)" 2>&1)
    Check "backup/cf-dmas-pcf-20260816 exists" ($branches -contains "backup/cf-dmas-pcf-20260816")
    Check "backup/sysdelay-per-channel-20260817 exists" ($branches -contains "backup/sysdelay-per-channel-20260817")

    Write-Host "`ngit status --short:"
    & $GitExe @gitArgs -c core.quotepath=false status --short
    Write-Host '(New commits after the imported baseline are allowed; inspect working changes separately.)'
}

Write-Host "`n== 2. Build cache path check =="
$cache = Join-Path $buildDir "CMakeCache.txt"
Check "new CMakeCache.txt exists" (Test-Path -LiteralPath $cache)
if (Test-Path -LiteralPath $cache) {
    # CMakeCache 内路径统一为 '/', 但 OldRoot 参数可能给 '\'; 两种分隔符都查
    $oldForward = $OldRoot.Replace('\', '/')
    $oldBack    = $OldRoot.Replace('/', '\')
    $staleHits = @(
        Select-String -Path $cache -Pattern $oldForward -SimpleMatch -ErrorAction SilentlyContinue
        Select-String -Path $cache -Pattern $oldBack -SimpleMatch -ErrorAction SilentlyContinue
    ) | Where-Object { $_ }
    Check "no old root path in CMakeCache" ($staleHits.Count -eq 0) ("hits=$($staleHits.Count)")
    $sourceHomeLine = (Select-String -Path $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=' | Select-Object -First 1).Line
    Write-Host "CMAKE_HOME_DIRECTORY -> $sourceHomeLine"
    $expectHome = "CMAKE_HOME_DIRECTORY:INTERNAL=$($delivery.Replace('\', '/'))"
    Check "CMAKE_HOME_DIRECTORY points to new workspace" ($sourceHomeLine -eq $expectHome) "expected: $expectHome"
    $cacheDirLine = (Select-String -LiteralPath $cache -Pattern '^CMAKE_CACHEFILE_DIR:INTERNAL=' | Select-Object -First 1).Line
    Check 'receiver cache points to new workspace' ($cacheDirLine -eq "CMAKE_CACHEFILE_DIR:INTERNAL=$($buildDir.Replace('\', '/'))")
}

$simSource = Join-Path $WorkspaceRoot 'PALiveImagingSimSender'
$simCache = Join-Path $simSource 'build\CMakeCache.txt'
Check 'simulator CMakeCache exists' (Test-Path -LiteralPath $simCache)
if (Test-Path -LiteralPath $simCache) {
    $simHome = (Select-String -LiteralPath $simCache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=' | Select-Object -First 1).Line
    Check 'simulator source points to new workspace' ($simHome -eq "CMAKE_HOME_DIRECTORY:INTERNAL=$($simSource.Replace('\', '/'))")
    $simBuildPath = (Join-Path $simSource 'build').Replace('\', '/')
    $simCacheDir = (Select-String -LiteralPath $simCache -Pattern '^CMAKE_CACHEFILE_DIR:INTERNAL=' | Select-Object -First 1).Line
    Check 'simulator cache points to new workspace' ($simCacheDir -eq "CMAKE_CACHEFILE_DIR:INTERNAL=$simBuildPath")
}

Write-Host "`n== 3. Key artifacts =="
Check "MC410T_Receiver.exe" (Test-Path -LiteralPath (Join-Path $binDir "MC410T_Receiver.exe"))
Check "ImagingSvc.exe"        (Test-Path -LiteralPath (Join-Path $binDir "ImagingSvc.exe"))
Check "ring_svc_selftest.exe" (Test-Path -LiteralPath (Join-Path $binDir "ring_svc_selftest.exe"))
Check 'ring_udp_replay.exe' (Test-Path -LiteralPath (Join-Path $binDir 'ring_udp_replay.exe'))
Check 'PALiveImagingSimSender.exe' (Test-Path -LiteralPath (Join-Path $simSource 'build\bin\PALiveImagingSimSender.exe'))
Check "ring_recon_cuda.dll"   (Test-Path -LiteralPath (Join-Path $binDir "ring_recon_cuda.dll"))
Check "cudart64_12.dll"       (Test-Path -LiteralPath (Join-Path $binDir "cudart64_12.dll"))
foreach ($file in @('ring_recon_cuda.dll', 'cudart64_12.dll')) {
    $actual = Join-Path $binDir $file
    $reference = Join-Path $PSScriptRoot "prebuilt_cuda\bin\$file"
    $equal = (Test-Path -LiteralPath $actual) -and (Test-Path -LiteralPath $reference)
    if ($equal) { $equal = (Get-FileHash -LiteralPath $actual).Hash -eq (Get-FileHash -LiteralPath $reference).Hash }
    Check "$file matches migration prebuilt" $equal
}

Write-Host "`n== 4. Registry sampling-rate keys (same-machine migration only) =="
try {
    $s = (Get-ItemProperty -Path "HKCU:\Software\MC410T\MC410T_Receiver\AcquisitionParams" -ErrorAction Stop).SampleIntervalNs
    $d = (Get-ItemProperty -Path "HKCU:\Software\MC410T\MC410T_Receiver\ImagingParams\General" -ErrorAction Stop).DaqHz
    Check "SampleIntervalNs == '4.0'" ($s -eq "4.0") "value=$s"
    Check "DaqHz == '2.5e+08'"        ($d -eq "2.5e+08") "value=$d"
} catch {
    $executionAccount = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    Write-Host "[INFO] Registry keys unavailable for current account ($executionAccount); this is not a check of another user's HKCU. Program defaults remain 250MHz (4.0ns)."
}

Write-Host "`n== Result =="
if ($failures -eq 0) {
    Write-Host "ALL CHECKS PASSED"
    exit 0
} else {
    Write-Host "$failures check(s) failed"
    exit 1
}
