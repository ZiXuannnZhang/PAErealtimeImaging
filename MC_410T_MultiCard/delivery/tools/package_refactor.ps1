[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')]
    [string]$Configuration = 'Release',
    [string]$Baseline = '9d0fd7a84b3725a3cfa6e1aefb9be2559add0cda'
)

$ErrorActionPreference = 'Stop'
$Product = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Repo = (Resolve-Path (Join-Path $Product '..\..')).Path
$build = Join-Path $Product ('build\refactor_' + $Configuration.ToLowerInvariant() + '\bin')
$artifactRoot = Join-Path $Repo 'artifacts\RingPipelineRefactor'
$artifact = Join-Path $artifactRoot 'bin'
if (-not (Test-Path -LiteralPath $build)) { throw ('missing build output: ' + $build) }

# The artifact directory is generated output owned by this script.  Recreate
# it so repeated packaging cannot nest a previous plugin directory into the
# new package.
if (Test-Path -LiteralPath $artifact) {
    $resolvedArtifact = (Resolve-Path -LiteralPath $artifact).Path
    $expectedArtifact = [System.IO.Path]::GetFullPath($artifact)
    if (-not $resolvedArtifact.Equals($expectedArtifact, [System.StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path $resolvedArtifact -Leaf) -ne 'bin' -or
        (Split-Path (Split-Path $resolvedArtifact -Parent) -Leaf) -ne 'RingPipelineRefactor') {
        throw ('refusing to clean unexpected artifact path: ' + $resolvedArtifact)
    }
    Remove-Item -LiteralPath $resolvedArtifact -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $artifact | Out-Null
Get-ChildItem -LiteralPath $build -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $artifact $_.Name) -Force
}
Get-ChildItem -LiteralPath $build -Directory | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $artifact $_.Name) -Recurse -Force
}

$head = (& git -C $Repo rev-parse HEAD).Trim()
$status = (& git -C $Repo status --porcelain)
$files = @()
Get-ChildItem -LiteralPath $artifact -File -Recurse | ForEach-Object {
    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    $files += [ordered]@{ path=$_.FullName.Substring($artifact.Length + 1); sha256=$hash; bytes=$_.Length }
}
$manifest = [ordered]@{
    schemaVersion = 1
    baselineCommit = $Baseline
    implementationHead = $head
    worktreeDirty = ($status.Count -gt 0)
    configuration = $Configuration
    compiler = 'D:\Qt\Qt6.8.0\Tools\mingw1310_64\bin\g++.exe'
    qt = 'D:\Qt\Qt6.8.0\6.8.0\mingw_64'
    ipcVersion = 3
    dependencySource = '_migration_pack/prebuilt_cuda plus delivery/third_party/zeromq and Qt deployment'
    tests = (Join-Path $Repo 'CODEX_REPORTS\ring-pipeline-refactor\test-summary.json')
    files = $files
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Repo 'artifacts\RingPipelineRefactor\build-manifest.json') -Encoding UTF8
Write-Host ('packaged ' + $files.Count + ' files to ' + $artifact)
exit 0
