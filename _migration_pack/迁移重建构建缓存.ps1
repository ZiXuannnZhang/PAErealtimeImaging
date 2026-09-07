# 迁移重建构建缓存.ps1
# 用途：CMake 生成物（CMakeCache.txt、build.ninja/Makefile、*.obj 等）包含旧工作区的
#       绝对路径，复制到新工作区后不能直接使用。本脚本把旧 build 目录改名保留，
#       用“新工作区路径”重新 configure 并重建主程序三目标。
#
# 用法（在新工作区中执行）：
#   pwsh -ExecutionPolicy Bypass -File .\_migration_pack\迁移重建构建缓存.ps1
#   pwsh -ExecutionPolicy Bypass -File .\_migration_pack\迁移重建构建缓存.ps1 -QtRoot "E:\Qt\Qt6.8.0"
#
# 说明：
#   - 不删除旧 build，只改名 build_oldpath_<时间戳>，方便回看；
#   - CUDA 不重编：直接使用迁移包里的 prebuilt_cuda\bin（当前未提交代码没有改 .cu/.h，
#     该 DLL 是 b6b00e7 源码构建的有效产物）；以后改 CUDA 源码再按 HANDOFF 第 7.2 节重建。

param(
    [string]$QtRoot = "D:\Qt\Qt6.8.0",
    [string]$CmakeExe = "",
    [string]$WorkspaceRoot = "",
    [switch]$ForceStopApps
)

$ErrorActionPreference = "Stop"

if (-not $WorkspaceRoot) {
    $WorkspaceRoot = Split-Path -Parent $PSScriptRoot
}
$delivery = Join-Path $WorkspaceRoot "MC_410T_MultiCard\delivery"
$buildDir = Join-Path $delivery "build\mingw_make"
$prebuiltSrc = Join-Path $PSScriptRoot "prebuilt_cuda\bin"
$prebuiltDst = Join-Path $delivery "third_party\ring_recon_cuda_prebuilt\bin"

Write-Host "== Reconfigure CMake build cache for new workspace =="
Write-Host "WorkspaceRoot : $WorkspaceRoot"
Write-Host "Delivery      : $delivery"

if (-not (Test-Path -LiteralPath $delivery)) {
    throw "Delivery source directory missing: $delivery"
}

# 1. 结束可能锁住 exe 的进程
$running = Get-Process -ErrorAction SilentlyContinue |
    Where-Object { $_.ProcessName -match '^(MC410T_Receiver|ImagingSvc|ring_svc_selftest|ring_udp_replay)$' }
if ($running) {
    if ($ForceStopApps) {
        $running | Stop-Process -Force
        Write-Host "Stopped running apps: $($running.ProcessName -join ', ')"
        Start-Sleep -Seconds 1
    } else {
        throw "Running apps lock output files: $($running.ProcessName -join ', '). Stop them or rerun with -ForceStopApps."
    }
}

# 2. 先保留 CUDA 预构建产物到源码树（路径独立）
if (Test-Path -LiteralPath $prebuiltSrc) {
    New-Item -ItemType Directory -Path $prebuiltDst -Force | Out-Null
    Copy-Item -Path (Join-Path $prebuiltSrc "*") -Destination $prebuiltDst -Force
    Write-Host "CUDA prebuilt artifacts copied to: $prebuiltDst"
} else {
    throw "Prebuilt CUDA dir missing: $prebuiltSrc. Re-transfer the whole _migration_pack folder."
}

# 3. 把引用旧绝对路径的 build 缓存改名保留，不直接删
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$staleBuilds = @(
    (Join-Path $delivery "build\mingw_make"),
    (Join-Path $delivery "build\mingw_debug"),
    (Join-Path $delivery "build\mingw_asan"),
    (Join-Path $delivery "build\msvc2022_release"),
    (Join-Path $delivery "build\ring_recon_cuda"),
    (Join-Path $delivery "build\ring_recon_release"),
    (Join-Path $delivery "build\ring_recon_debug")
)
foreach ($b in $staleBuilds) {
    if (Test-Path -LiteralPath $b) {
        Rename-Item -LiteralPath $b -NewName ("{0}_oldpath_{1}" -f (Split-Path -Leaf $b), $stamp)
        Write-Host "Renamed stale cache: $b -> $($b)_oldpath_$stamp"
    }
}
$simBuild = Join-Path $WorkspaceRoot "PALiveImagingSimSender\build"
if (Test-Path -LiteralPath $simBuild) {
    Rename-Item -LiteralPath $simBuild -NewName ("build_oldpath_{0}" -f $stamp)
    Write-Host "Renamed stale simulator cache: $simBuild"
}

# 4. 定位工具链（新机器 Qt 路径可能不同，用 -QtRoot 覆盖）
$mingwBin = Join-Path $QtRoot "Tools\mingw1310_64\bin"
$qtCmake  = Join-Path $QtRoot "6.8.0\mingw_64\lib\cmake\Qt6"
$cmakeCandidates = @()
if ($CmakeExe) { $cmakeCandidates += $CmakeExe }
$cmakeCandidates += Join-Path $QtRoot "Tools\CMake_64\bin\cmake.exe"
$cmakeCandidates += "cmake"
$cmake = $null
foreach ($c in $cmakeCandidates) {
    if ($c -eq "cmake") {
        $probe = Get-Command cmake -ErrorAction SilentlyContinue
        if ($probe) { $cmake = $probe.Source; break }
    } elseif (Test-Path -LiteralPath $c) {
        $cmake = $c; break
    }
}
if (-not $cmake) { throw "cmake not found. Install it or pass -CmakeExe." }

$gcc   = Join-Path $mingwBin "gcc.exe"
$gxx   = Join-Path $mingwBin "g++.exe"
$make  = Join-Path $mingwBin "mingw32-make.exe"
foreach ($t in @($gcc, $gxx, $make)) {
    if (-not (Test-Path -LiteralPath $t)) {
        throw "Toolchain missing: $t. Check -QtRoot (current: $QtRoot)."
    }
}
if (-not (Test-Path -LiteralPath $qtCmake)) {
    throw "Qt6_DIR missing: $qtCmake"
}

# 5. 用新路径 configure（全新 build 目录）
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
$importLib = Join-Path $prebuiltDst "libring_recon_cuda.dll.a"
$cudaBin   = $prebuiltDst

$configureArgs = @(
    "-S", $delivery,
    "-B", $buildDir,
    "-G", "MinGW Makefiles",
    "-DCMAKE_BUILD_TYPE=Debug",
    "-DCMAKE_C_COMPILER=$gcc",
    "-DCMAKE_CXX_COMPILER=$gxx",
    "-DCMAKE_MAKE_PROGRAM=$make",
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    "-DQt6_DIR=$qtCmake",
    "-DUSE_ZEROMQ=ON",
    "-DZMQ_INSTALL=$(Join-Path $delivery 'third_party\zeromq')",
    "-DIMAGING_ZMQ_DIR=$(Join-Path $delivery 'third_party\zeromq')",
    "-DRING_RECON_CUDA_IMPORT_LIB=$importLib",
    "-DRING_RECON_CUDA_BIN=$cudaBin"
)
Write-Host "`nConfigure command:"
Write-Host ("& `"{0}`" {1}" -f $cmake, ($configureArgs -join ' '))
& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)." }

# 6. 重建三目标
$buildArgs = @("--build", $buildDir, "--target", "ImagingSvc", "MC410T_Receiver", "ring_svc_selftest", "-j", "8")
Write-Host "`nBuild command:"
Write-Host ("& `"{0}`" {1}" -f $cmake, ($buildArgs -join ' '))
& $cmake @buildArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE). See output above." }

# 7. 确保 CUDA DLL/运行时位于 bin（post-build 通常也会复制）
$binOut = Join-Path $buildDir "bin"
foreach ($f in @("ring_recon_cuda.dll", "cudart64_12.dll")) {
    Copy-Item -Path (Join-Path $prebuiltDst $f) -Destination $binOut -Force -ErrorAction SilentlyContinue
}

Write-Host "`n== Done. Artifacts =="
Get-Item (Join-Path $binOut "MC410T_Receiver.exe"),
         (Join-Path $binOut "ImagingSvc.exe"),
         (Join-Path $binOut "ring_svc_selftest.exe") |
    Select-Object Name, Length, LastWriteTime

Write-Host "`nNext: run .\_migration_pack\迁移后验证.ps1 to verify git + build cache + key outputs."
