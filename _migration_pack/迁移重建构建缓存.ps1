# 迁移重建构建缓存.ps1
# 用途：CMake 生成物（CMakeCache.txt、build.ninja/Makefile、*.obj 等）包含旧工作区的
#       绝对路径，复制到新工作区后不能直接使用。本脚本把旧 build 目录改名保留，
#       用“新工作区路径”重新 configure 并重建主程序三目标。
#
# 用法（在新工作区中执行）：
#   pwsh -ExecutionPolicy Bypass -File .\_migration_pack\迁移重建构建缓存.ps1
#   pwsh -ExecutionPolicy Bypass -File .\_migration_pack\迁移重建构建缓存.ps1 -QtRoot "E:\Qt\Qt6.8.0"
#   pwsh -ExecutionPolicy Bypass -File .\_migration_pack\迁移重建构建缓存.ps1 -ImagingRuntimeDir "D:\artifacts\CardDiscoveryFix"
#
# 说明：
#   - 不删除旧 build，只改名 build_oldpath_<时间戳>，方便回看；
#   - CUDA 不重编：直接使用迁移包里的 prebuilt_cuda\bin（当前未提交代码没有改 .cu/.h，
#     该 DLL 是 b6b00e7 源码构建的有效产物）；以后改 CUDA 源码再按 HANDOFF 第 7.2 节重建。

param(
    [string]$QtRoot = "D:\Qt\Qt6.8.0",
    [string]$CmakeExe = "",
    [string]$WorkspaceRoot = "",
    [string]$ImagingRuntimeDir = "",
    [switch]$ForceStopApps,
    [switch]$CheckOnly
)

$ErrorActionPreference = "Stop"

if (-not $WorkspaceRoot) {
    $WorkspaceRoot = Split-Path -Parent $PSScriptRoot
}
$WorkspaceRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
$delivery = Join-Path $WorkspaceRoot "MC_410T_MultiCard\delivery"
$buildDir = Join-Path $delivery "build\mingw_make"
$prebuiltSrc = Join-Path $PSScriptRoot "prebuilt_cuda\bin"
$prebuiltDst = Join-Path $delivery "third_party\ring_recon_cuda_prebuilt\bin"
if (-not $ImagingRuntimeDir) {
    $ImagingRuntimeDir = Join-Path $delivery "libs\imaging"
}
if (-not (Test-Path -LiteralPath $ImagingRuntimeDir -PathType Container)) {
    throw "Imaging runtime directory missing: $ImagingRuntimeDir"
}
$ImagingRuntimeDir = (Resolve-Path -LiteralPath $ImagingRuntimeDir).Path

Write-Host "== Reconfigure CMake build cache for new workspace =="
Write-Host "WorkspaceRoot : $WorkspaceRoot"
Write-Host "Delivery      : $delivery"
Write-Host "ImagingRuntime: $ImagingRuntimeDir"

if (-not (Test-Path -LiteralPath $delivery)) {
    throw "Delivery source directory missing: $delivery"
}

# 完整事前检查：任何复制/改名之前确认工具链、输入库和目标路径。
$preflightCmake = $CmakeExe
if (-not $preflightCmake) { $preflightCmake = Join-Path $QtRoot 'Tools\CMake_64\bin\cmake.exe' }
$required = @($preflightCmake,
    (Join-Path $QtRoot 'Tools\mingw1310_64\bin\gcc.exe'),
    (Join-Path $QtRoot 'Tools\mingw1310_64\bin\g++.exe'),
    (Join-Path $QtRoot 'Tools\mingw1310_64\bin\mingw32-make.exe'),
    (Join-Path $QtRoot '6.8.0\mingw_64\bin\windeployqt.exe'),
    (Join-Path $QtRoot '6.8.0\mingw_64\bin\moc.exe'),
    (Join-Path $QtRoot '6.8.0\mingw_64\bin\uic.exe'),
    (Join-Path $delivery 'third_party\zeromq\include\zmq.hpp'),
    (Join-Path $delivery 'third_party\zeromq\lib\libzmq.dll.a'),
    (Join-Path $delivery 'third_party\zeromq\bin\libzmq-v141-mt-4_3_5.dll'),
    (Join-Path $delivery 'libs\imaging\pa_recon_core.lib'),
    (Join-Path $ImagingRuntimeDir 'pa_recon_core.dll'),
    (Join-Path $ImagingRuntimeDir 'cufft64_12.dll'),
    (Join-Path $delivery 'CMakeLists.txt'),
    (Join-Path $WorkspaceRoot 'PALiveImagingSimSender\CMakeLists.txt'))
foreach ($component in @('Core','Gui','Widgets','PrintSupport','OpenGL','OpenGLWidgets','Svg','Concurrent')) {
    $required += Join-Path $QtRoot "6.8.0\mingw_64\lib\cmake\Qt6$component\Qt6${component}Config.cmake"
}
foreach ($file in @('ring_recon_cuda.dll','libring_recon_cuda.dll.a','cudart64_12.dll')) {
    $required += Join-Path $prebuiltSrc $file
}
foreach ($path in $required) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required input missing: $path" }
}
$CmakeExe = $preflightCmake
function Assert-InWorkspace([string]$path) {
    $full = [IO.Path]::GetFullPath($path)
    if (-not $full.StartsWith($WorkspaceRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path outside workspace: $full"
    }
    if ((Test-Path -LiteralPath $full) -and
        ((Get-Item -LiteralPath $full -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing to modify reparse point: $full"
    }
}
foreach ($path in @($delivery, (Join-Path $delivery 'build'), $prebuiltDst,
    (Join-Path $WorkspaceRoot 'PALiveImagingSimSender'), (Join-Path $WorkspaceRoot 'PALiveImagingSimSender\build'))) {
    Assert-InWorkspace $path
}
Write-Host 'Toolchain and input preflight: PASS'
if ($CheckOnly) { return }

# 1. 结束可能锁住 exe 的进程
$running = Get-Process -ErrorAction SilentlyContinue |
    Where-Object { $_.ProcessName -match '^(MC410T_Receiver|ImagingSvc|ring_svc_selftest|ring_udp_replay|PALiveImagingSimSender)$' }
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
$staleBuilds = @(Get-ChildItem -LiteralPath (Join-Path $delivery 'build') -Directory |
    Where-Object { $_.Name -notmatch '_oldpath_' -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'CMakeCache.txt')) } |
    Select-Object -ExpandProperty FullName)
if ((Test-Path -LiteralPath $buildDir) -and $staleBuilds -notcontains $buildDir) { $staleBuilds += $buildDir }
# 先检查全部改名目标，再执行任何改名。
foreach ($b in $staleBuilds) {
    Assert-InWorkspace $b
    Assert-InWorkspace "${b}_oldpath_$stamp"
    if (Test-Path -LiteralPath "${b}_oldpath_$stamp") { throw "Archive already exists: $b" }
}
foreach ($b in $staleBuilds) {
    if (Test-Path -LiteralPath $b) {
        Rename-Item -LiteralPath $b -NewName ("{0}_oldpath_{1}" -f (Split-Path -Leaf $b), $stamp)
        Write-Host "Renamed stale cache: $b -> $($b)_oldpath_$stamp"
    }
}
$simBuild = Join-Path $WorkspaceRoot "PALiveImagingSimSender\build"
if (Test-Path -LiteralPath $simBuild) {
    Assert-InWorkspace $simBuild
    Assert-InWorkspace "${simBuild}_oldpath_$stamp"
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
$env:PATH = "$(Join-Path $QtRoot '6.8.0\mingw_64\bin');$mingwBin;$env:PATH"
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
    "-DIMAGING_RUNTIME_DIR=$ImagingRuntimeDir",
    "-DRING_RECON_CUDA_IMPORT_LIB=$importLib",
    "-DRING_RECON_CUDA_BIN=$cudaBin"
)
Write-Host "`nConfigure command:"
Write-Host ("& `"{0}`" {1}" -f $cmake, ($configureArgs -join ' '))
& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)." }

# 6. 重建三目标
$buildArgs = @("--build", $buildDir, "--target", "ImagingSvc", "MC410T_Receiver", "ring_svc_selftest", "ring_udp_replay", "-j", "8")
Write-Host "`nBuild command:"
Write-Host ("& `"{0}`" {1}" -f $cmake, ($buildArgs -join ' '))
& $cmake @buildArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE). See output above." }

# 模拟器也需要新路径缓存与独立构建，不能仅改名旧目录。
& $cmake -S (Join-Path $WorkspaceRoot 'PALiveImagingSimSender') -B $simBuild `
    -G 'MinGW Makefiles' '-DCMAKE_BUILD_TYPE=Debug' "-DCMAKE_CXX_COMPILER=$gxx" `
    "-DCMAKE_MAKE_PROGRAM=$make" "-DQt6_DIR=$qtCmake"
if ($LASTEXITCODE -ne 0) { throw 'Simulator CMake configure failed.' }
& $cmake --build $simBuild --target PALiveImagingSimSender -j 8
if ($LASTEXITCODE -ne 0) { throw 'Simulator build failed.' }

# 7. 确保 CUDA DLL/运行时位于 bin（post-build 通常也会复制）
$binOut = Join-Path $buildDir "bin"
foreach ($f in @("ring_recon_cuda.dll", "cudart64_12.dll")) {
    Copy-Item -LiteralPath (Join-Path $prebuiltDst $f) -Destination $binOut -Force
}

Write-Host "`n== Done. Artifacts =="
Get-Item (Join-Path $binOut "MC410T_Receiver.exe"),
         (Join-Path $binOut "ImagingSvc.exe"),
         (Join-Path $binOut "ring_svc_selftest.exe") |
    Select-Object Name, Length, LastWriteTime

Write-Host "`nNext: run .\_migration_pack\迁移后验证.ps1 to verify git + build cache + key outputs."
