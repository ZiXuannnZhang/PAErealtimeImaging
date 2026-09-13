param(
    [string]$Worktree = (Split-Path -Parent $PSScriptRoot),
    [switch]$NoWpr,
    [string]$CaseName = 'synthetic'
)

$ErrorActionPreference = 'Stop'
$tools = Join-Path $Worktree 'MC_410T_MultiCard/delivery/tools'
$positiveName = if ($CaseName -eq 'synthetic') { 'system-capture-synthetic-positive' } else { "system-capture-synthetic-$CaseName-positive" }
$negativeName = if ($CaseName -eq 'synthetic') { 'system-capture-negative-nonadmin' } else { "system-capture-negative-nonadmin-$CaseName" }
$verificationRoot = Join-Path $Worktree ("verification/{0}" -f $positiveName)
$negativeRoot = Join-Path $Worktree ("verification/{0}" -f $negativeName)
foreach ($path in @($verificationRoot,$negativeRoot)) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
    New-Item -ItemType Directory -Path $path -Force | Out-Null
}
$adapter = Join-Path $PSScriptRoot 'system-capture-synthetic-adapter.ps1'
$start = Join-Path $tools 'start_system_capture_admin.ps1'
$common = Join-Path $tools 'system_capture_common.ps1'
. $common

$trial = 'synthetic-' + [Guid]::NewGuid().ToString('N')
$token = [Guid]::NewGuid().ToString('N')
$channel = Join-Path $verificationRoot 'system-capture-channel'
$output = Join-Path $verificationRoot 'capture'
New-Item -ItemType Directory -Path $channel,$output -Force | Out-Null
$request = [ordered]@{schemaVersion=1;kind='system-capture-request';trialId=$trial;captureSessionToken=$token;runId='run-synthetic-001';listenId='listen-synthetic-001';measurementSessionId='measurement-synthetic-001';cardIPs=@('192.168.0.1');ports=@(8000,8001,8002,8003,8004);outputDirectory=$output;channelDirectory=$channel;createdUtc=(Get-CaptureUtcNow).ToString('o')}
Write-CaptureAtomicJson (Join-Path $channel 'capture-request-synthetic.json') $request

$adapterRoot = Join-Path $verificationRoot 'adapter'
$env:SYSTEM_CAPTURE_TEST_ROOT = $adapterRoot
$ps = (Get-Command pwsh.exe).Source
$stdoutPath = Join-Path $verificationRoot 'start.stdout.txt'
$stderrPath = Join-Path $verificationRoot 'start.stderr.txt'
$startArgs = @('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',$start,'-OutputDirectory',$output,'-TrialId',$trial,'-ChannelDir',$channel,'-CardIPs','192.168.0.1','-RunId','run-synthetic-001','-ListenId','listen-synthetic-001','-MeasurementSessionId','measurement-synthetic-001','-CaptureSessionToken',$token,'-AdapterPath',$adapter,'-MaxWaitSeconds','20')
if ($NoWpr) { $startArgs += '-NoWpr' }
$process = Start-Process -FilePath $ps -ArgumentList $startArgs -WorkingDirectory $Worktree -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -PassThru
$readyPath = Join-Path $output 'capture-ready.json'
$ready = $null
for ($i=0; $i -lt 100; ++$i) {
    Start-Sleep -Milliseconds 200
    $ready = Read-CaptureJson $readyPath
    if ($null -ne $ready) { break }
    if ($process.HasExited) { break }
}
if ($null -eq $ready) {
    $process.WaitForExit(1000) | Out-Null
    throw ('synthetic capture did not reach ready: ' + (Get-Content -Raw $stderrPath -ErrorAction SilentlyContinue))
}
$burst = [ordered]@{schemaVersion=2;kind='burst';trialId=$trial;captureSessionToken=$token;runId='run-synthetic-001';listenId='listen-synthetic-001';measurementSessionId='measurement-synthetic-001';burstMonotonicNs=([Int64]$ready.readyMonotonicNs + 1000000);burstUtc=(Get-CaptureUtcNow).ToString('o');packetCount=2}
Write-CaptureAtomicJson (Join-Path $channel 'burst-synthetic.json') $burst
if (-not $process.WaitForExit(30000)) { $process.Kill(); throw 'synthetic capture process did not finish' }
$statePath = Join-Path $output 'capture-state.json'
$state = Read-CaptureJson $statePath
$manifest = Read-CaptureJson (Join-Path $output 'system-capture-manifest.json')
if ($process.ExitCode -ne 0 -or $null -eq $state -or [string]$state.status -ne 'complete' -or $null -eq $manifest -or -not [bool]$manifest.targetPacketsPresent) {
    throw ('synthetic capture failed: exit=' + $process.ExitCode + '; state=' + ([string]$state.status) + '; stderr=' + (Get-Content -Raw $stderrPath -ErrorAction SilentlyContinue))
}
$expectedLayer = if ($NoWpr) { 'pktmon_only_wpr_skipped' } else { 'pktmon_plus_wpr' }
if ([string]$manifest.layerCoverage -ne $expectedLayer) {
    throw ('unexpected layer coverage: expected=' + $expectedLayer + '; actual=' + [string]$manifest.layerCoverage)
}

$negativeTrial = 'negative-' + [Guid]::NewGuid().ToString('N')
$negativeToken = [Guid]::NewGuid().ToString('N')
$negativeChannel = Join-Path $negativeRoot 'system-capture-channel'
$negativeOutput = Join-Path $negativeRoot 'capture'
New-Item -ItemType Directory -Path $negativeChannel,$negativeOutput -Force | Out-Null
Write-CaptureAtomicJson (Join-Path $negativeChannel 'capture-request-negative.json') ([ordered]@{schemaVersion=1;kind='system-capture-request';trialId=$negativeTrial;captureSessionToken=$negativeToken;runId='run-negative';listenId='listen-negative';measurementSessionId='measurement-negative';cardIPs=@('192.168.0.1');ports=@(8000,8001,8002,8003,8004);outputDirectory=$negativeOutput;channelDirectory=$negativeChannel;createdUtc=(Get-CaptureUtcNow).ToString('o')})
$env:SYSTEM_CAPTURE_TEST_ROOT = $null
$negativeArgs = @('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',$start,'-OutputDirectory',$negativeOutput,'-TrialId',$negativeTrial,'-ChannelDir',$negativeChannel,'-CardIPs','192.168.0.1','-RunId','run-negative','-ListenId','listen-negative','-MeasurementSessionId','measurement-negative','-CaptureSessionToken',$negativeToken,'-MaxWaitSeconds','1')
$negative = Start-Process -FilePath $ps -ArgumentList $negativeArgs -WorkingDirectory $Worktree -Wait -PassThru -RedirectStandardOutput (Join-Path $negativeRoot 'stdout.txt') -RedirectStandardError (Join-Path $negativeRoot 'stderr.txt')
$negativeState = Read-CaptureJson (Join-Path $negativeOutput 'capture-state.json')
if ($negative.ExitCode -eq 0 -or $null -eq $negativeState -or [string]$negativeState.status -ne 'failed' -or @($negativeState.failureReasons | Where-Object { [string]$_ -match 'administrator privileges' }).Count -eq 0) {
    throw ('negative non-admin validation failed: exit=' + $negative.ExitCode)
}

$manifestRelative = "$positiveName/capture/system-capture-manifest.json"
$result = [ordered]@{schemaVersion=1;caseName=$CaseName;noWpr=[bool]$NoWpr;positive=[ordered]@{status=$state.status;exitCode=$process.ExitCode;targetPacketsPresent=$manifest.targetPacketsPresent;analysisReady=$manifest.analysisReady;layerCoverage=$manifest.layerCoverage;manifest=$manifestRelative};negative=[ordered]@{status=$negativeState.status;exitCode=$negative.ExitCode;reason='administrator privileges are required for system capture'};completedUtc=(Get-CaptureUtcNow).ToString('o')}
$resultPath = if ($CaseName -eq 'synthetic') { Join-Path $Worktree 'verification/system-capture-synthetic-result.json' } else { Join-Path $Worktree ("verification/system-capture-$CaseName-result.json") }
Write-CaptureAtomicJson $resultPath $result
$result | ConvertTo-Json -Depth 20
