[CmdletBinding()]
param(
    [ValidateSet('Debug','Release','Both')]
    [string]$Configuration = 'Debug',
    [string]$CMake = 'D:\Qt\Qt6.8.0\Tools\CMake_64\bin\cmake.exe',
    [string]$Ninja = 'D:\Qt\Qt6.8.0\Tools\Ninja\ninja.exe',
    [string]$Compiler = 'D:\Qt\Qt6.8.0\Tools\mingw1310_64\bin\g++.exe',
    [string]$QtDir = 'D:\Qt\Qt6.8.0\6.8.0\mingw_64\lib\cmake\Qt6'
)

$ErrorActionPreference = 'Stop'
$Product = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Repo = (Resolve-Path (Join-Path $Product '..\..')).Path
$ReportDir = Join-Path $Repo 'CODEX_REPORTS\ring-pipeline-refactor'
New-Item -ItemType Directory -Force -Path $ReportDir | Out-Null

function Invoke-NativeStep([string]$Name, [string[]]$Arguments) {
    $log = Join-Path $ReportDir ("build-" + $Name + ".log")
    Write-Host ("== " + $Name + " ==")
    & $CMake @Arguments *> $log
    $code = $LASTEXITCODE
    Get-Content -LiteralPath $log
    if ($code -ne 0) { throw ("step failed: " + $Name + " (exit " + $code + ")") }
}

foreach ($path in @($CMake, $Ninja, $Compiler, (Join-Path $QtDir 'Qt6Config.cmake'))) {
    if (-not (Test-Path -LiteralPath $path)) { throw ("missing tool or Qt path: " + $path) }
}

$common = @(
    '-G', 'Ninja',
    ('-DCMAKE_MAKE_PROGRAM=' + $Ninja),
    ('-DCMAKE_CXX_COMPILER=' + $Compiler),
    ('-DQt6_DIR=' + $QtDir)
)
Invoke-NativeStep 'refactor-tests-configure' (@('-S', (Join-Path $Product 'tests'), '-B', (Join-Path $Product 'build\refactor_tests')) + $common + @('-DCMAKE_BUILD_TYPE=Debug'))
Invoke-NativeStep 'refactor-tests-build' @('--build', (Join-Path $Product 'build\refactor_tests'), '--parallel', '2')

$configs = if ($Configuration -eq 'Both') { @('Debug','Release') } else { @($Configuration) }
$cudaBin = Join-Path $Repo '_migration_pack\prebuilt_cuda\bin'
$cudaLib = Join-Path $cudaBin 'libring_recon_cuda.dll.a'
$zmqDir = Join-Path $Product 'third_party\zeromq'
if (-not (Test-Path -LiteralPath $cudaLib)) { throw ("missing local CUDA import library: " + $cudaLib) }
if (-not (Test-Path -LiteralPath $zmqDir)) { throw ("missing local ZeroMQ directory: " + $zmqDir) }

foreach ($config in $configs) {
    $suffix = $config.ToLowerInvariant()
    $buildDir = Join-Path $Product ("build\refactor_" + $suffix)
    $args = @('-S', $Product, '-B', $buildDir) + $common + @(
        ('-DCMAKE_BUILD_TYPE=' + $config),
        '-DUSE_ZEROMQ=ON',
        ('-DIMAGING_ZMQ_DIR=' + $zmqDir),
        ('-DRING_RECON_CUDA_IMPORT_LIB=' + $cudaLib),
        ('-DRING_RECON_CUDA_BIN=' + $cudaBin)
    )
    Invoke-NativeStep ("refactor-" + $suffix + "-configure") $args
    Invoke-NativeStep ("refactor-" + $suffix + "-build") @('--build', $buildDir, '--parallel', '2')
}

Write-Host 'build_refactor completed'
exit 0
