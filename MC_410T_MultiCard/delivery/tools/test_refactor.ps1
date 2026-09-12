[CmdletBinding()]
param(
    [string]$BuildDir = '',
    [string]$CMake = 'D:\Qt\Qt6.8.0\Tools\CMake_64\bin\cmake.exe',
    [string]$QtBin = 'D:\Qt\Qt6.8.0\6.8.0\mingw_64\bin',
    [string]$MinGWBin = 'D:\Qt\Qt6.8.0\Tools\mingw1310_64\bin'
)

$ErrorActionPreference = 'Stop'
$Product = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Repo = (Resolve-Path (Join-Path $Product '..\..')).Path
$ReportDir = Join-Path $Repo 'CODEX_REPORTS\ring-pipeline-refactor'
New-Item -ItemType Directory -Force -Path $ReportDir | Out-Null
if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = Join-Path $Product 'build\refactor_tests' }
$BuildDir = (Resolve-Path $BuildDir).Path
$ctest = Join-Path (Split-Path $CMake) 'ctest.exe'
if (-not (Test-Path -LiteralPath $ctest)) { throw ('missing ctest: ' + $ctest) }
$env:PATH = ($QtBin + ';' + $MinGWBin + ';' + (Join-Path $BuildDir 'bin') + ';' + $env:PATH)

$listLog = Join-Path $ReportDir 'test-list.log'
& $ctest '--test-dir' $BuildDir '-N' *> $listLog
if ($LASTEXITCODE -ne 0) { Get-Content $listLog; exit $LASTEXITCODE }
$names = @()
foreach ($line in Get-Content -LiteralPath $listLog) {
    if ($line -match 'Test\s+#\d+:\s+(.+)$') { $names += $Matches[1].Trim() }
}
if ($names.Count -eq 0) { throw ('no tests discovered in ' + $BuildDir) }

$results = @()
$failed = $false
$index = 0
foreach ($name in $names) {
    ++$index
    $safe = ($name -replace '[^A-Za-z0-9_.-]', '_')
    $log = Join-Path $ReportDir ("test-{0:D3}-{1}.log" -f $index, $safe)
    & $ctest '--test-dir' $BuildDir '-R' ('^' + [regex]::Escape($name) + '$') '--output-on-failure' '--timeout' '120' '-j' '1' *> $log
    $code = $LASTEXITCODE
    $results += [ordered]@{ name=$name; exitCode=$code; log=$log }
    if ($code -ne 0) { $failed = $true; Get-Content -LiteralPath $log }
}
$summary = [ordered]@{
    schemaVersion = 1
    buildDir = $BuildDir
    testCount = $names.Count
    failed = @($results | Where-Object { $_.exitCode -ne 0 }).Count
    results = $results
}
$summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $ReportDir 'test-summary.json') -Encoding UTF8
if ($failed) { exit 1 }
Write-Host ("test_refactor passed " + $names.Count + " tests")
exit 0
