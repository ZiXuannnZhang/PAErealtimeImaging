param(
    [Parameter(Mandatory=$true)][string]$TrialId,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string[]]$CardIPs = @(),
    [int[]]$Ports = @(8000,8001,8002,8003,8004),
    [string]$ChannelDir = '',
    [int]$MaxWaitSeconds = 30,
    [string]$WprProfile = 'GeneralProfile',
    [switch]$SkipWpr
)
$ErrorActionPreference='Stop'

# Startup ingress system capture. Starts before the operator triggers a
# physical round, stops automatically when the application publishes a
# receive-burst notification (10 s after the burst) or after the timeout,
# then delegates stopping to startup_ingress_capture_stop.ps1.
# It never stops or overwrites sessions it did not create.

$prefix='StartupDiag'
$root=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $root -Force | Out-Null

function Invoke-Tool([string]$Program,[string[]]$ToolArgs,[string]$Description){
    $begin=[DateTime]::UtcNow
    $text=& $Program @ToolArgs 2>&1 | Out-String
    $exit=$LASTEXITCODE
    return [ordered]@{description=$Description;command=("$Program "+($ToolArgs -join ' '));
        exitCode=$exit;startUtc=$begin.ToString('o');endUtc=[DateTime]::UtcNow.ToString('o');
        output=$text}
}
$commands=[System.Collections.Generic.List[object]]::new()
$failureReasons=[System.Collections.Generic.List[string]]::new()
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
$principal=New-Object Security.Principal.WindowsPrincipal($identity)
$isAdmin=$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if(-not $isAdmin){ $failureReasons.Add('administrator privileges are required for pktmon status/filter/start') }

$wprStatus=Invoke-Tool 'wpr.exe' @('-status') 'check existing WPR sessions'
$commands.Add($wprStatus)
if($wprStatus.output -notmatch 'WPR is not recording'){ $failureReasons.Add('an existing WPR session is recording; this task refuses to stop or overwrite it') }

$filterNames=[System.Collections.Generic.List[string]]::new()
if($isAdmin){
    $pktmonStatus=Invoke-Tool 'pktmon.exe' @('status') 'check existing pktmon session'
    $commands.Add($pktmonStatus)
    if($pktmonStatus.exitCode -ne 0 -or $pktmonStatus.output -notmatch '(?i)(stopped|not running|已停止|未运行|未启动)'){
        $failureReasons.Add('pktmon status did not prove that no session is running; this task refuses to stop or overwrite it')
    }
    $filterList=Invoke-Tool 'pktmon.exe' @('filter','list') 'list existing pktmon filters'
    $commands.Add($filterList)
    if($filterList.exitCode -ne 0 -or $filterList.output -notmatch '(?i)(no .*filter|0 .*filter|没有.*筛选|无.*筛选)'){
        $failureReasons.Add('pktmon filter list did not prove that the filter set is empty; this task refuses to remove possibly foreign filters')
    }
}

if($failureReasons.Count -gt 0){
    $state=[ordered]@{trialId=$TrialId;outputDirectory=$root;pktmonStarted=$false;wprStarted=$false;
        filterNames=@();commands=$commands;failureReasons=$failureReasons;status='failed'}
    $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $root 'capture-state.json') -Encoding utf8
    [Console]::Error.WriteLine("capture start failed: " + ($failureReasons -join '; '))
    exit 2
}

# One UDP filter per port. When card IPs are known, each filter also pins the
# card IPv4 so only acquisition-card UDP traffic on the acquisition ports is
# captured; drop events of all components stay enabled.
for($portIndex=0;$portIndex -lt $Ports.Count;$portIndex++){
    $port=$Ports[$portIndex]
    if($CardIPs.Count -gt 0){
        for($ipIndex=0;$ipIndex -lt $CardIPs.Count;$ipIndex++){
            $name=("$prefix-Ip$ipIndex-p$port")
            $commands.Add((Invoke-Tool 'pktmon.exe' @('filter','add',$name,'-t','UDP','-p',[string]$port,'-i',$CardIPs[$ipIndex]) ("add acquisition UDP filter "+$name)))
            $filterNames.Add($name)
        }
    } else {
        $name="$prefix-Udp$port"
        $commands.Add((Invoke-Tool 'pktmon.exe' @('filter','add',$name,'-t','UDP','-p',[string]$port) ("add UDP port filter "+$name)))
        $filterNames.Add($name)
    }
}

$startedUtc=[DateTime]::UtcNow
$startedQpc=[System.Diagnostics.Stopwatch]::GetTimestamp()
$startedUtcMs=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()

$pktmonStart=Invoke-Tool 'pktmon.exe' @('start','--capture','--comp','all','--type','all','--pkt-size','128','--log-mode','memory','--file-size','256','--file-name',(Join-Path $root 'startup-pktmon.etl')) 'start pktmon memory capture (flow+drop, all components, 128-byte truncation)'
$commands.Add($pktmonStart)
$pktmonStarted=($pktmonStart.exitCode -eq 0)
if(-not $pktmonStarted){ $failureReasons.Add('pktmon start failed') }

$components='unknown'
if($pktmonStarted){
    $componentList=Invoke-Tool 'pktmon.exe' @('list','--json') 'list observable pktmon components'
    $commands.Add($componentList)
    $components=$componentList.output
}

$wprStarted=$false
if($pktmonStarted -and -not $SkipWpr){
    $wprStart=Invoke-Tool 'wpr.exe' @('-start',$WprProfile) ("start WPR memory profile $WprProfile")
    $commands.Add($wprStart)
    $wprStarted=($wprStart.exitCode -eq 0)
    if(-not $wprStarted){ $failureReasons.Add('wpr start failed') }
}

if(-not $pktmonStarted){
    $commands.Add((Invoke-Tool 'pktmon.exe' @('filter','remove') 'remove filter set created by this task after failed start'))
    $state=[ordered]@{trialId=$TrialId;outputDirectory=$root;pktmonStarted=$false;wprStarted=$false;
        filterNames=@();commands=$commands;failureReasons=$failureReasons;status='failed'}
    $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $root 'capture-state.json') -Encoding utf8
    [Console]::Error.WriteLine("capture start failed: " + ($failureReasons -join '; '))
    exit 2
}
if(-not $SkipWpr -and -not $wprStarted){
    $state=[ordered]@{trialId=$TrialId;outputDirectory=$root;pktmonStarted=$true;wprStarted=$false;
        filterNames=$filterNames;ports=$Ports;cardIPs=$CardIPs;wprProfile=$WprProfile;
        maxWaitSeconds=$MaxWaitSeconds;startedUtc=$startedUtc.ToString('o');startedQpc=$startedQpc;
        applicationRunId=$null;applicationWallAnchorMs=$null;notification=$null;
        stoppedBy='startup-failure';components=$components;commands=$commands;failureReasons=$failureReasons}
    $statePath=Join-Path $root 'capture-state.json'
    $state | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $statePath -Encoding utf8
    & (Join-Path $PSScriptRoot 'startup_ingress_capture_stop.ps1') -StatePath $statePath
    exit 2
}

# Publish the per-round identity for the already-running application. This
# permits a second START on the same listener to use a distinct trialId. The
# UI-thread notification reads this small file 10 s after detecting the burst;
# the receive thread never performs file I/O or waits for the script.
$activeTrialPath=$null
if($ChannelDir){
    try{
        New-Item -ItemType Directory -Path $ChannelDir -Force | Out-Null
        $activeTrialPath=Join-Path ([IO.Path]::GetFullPath($ChannelDir)) 'active-trial.json'
        [ordered]@{trialId=$TrialId;startedWallMs=$startedUtcMs;
            expiresWallMs=($startedUtcMs+($MaxWaitSeconds+20)*1000)} |
            ConvertTo-Json | Set-Content -LiteralPath $activeTrialPath -Encoding utf8
    } catch {
        $failureReasons.Add('failed to publish the active trial identity: '+$_.Exception.Message)
    }
}
if($failureReasons.Count -gt 0){
    $state=[ordered]@{trialId=$TrialId;outputDirectory=$root;pktmonStarted=$pktmonStarted;wprStarted=$wprStarted;
        filterNames=$filterNames;ports=$Ports;cardIPs=$CardIPs;wprProfile=$WprProfile;
        maxWaitSeconds=$MaxWaitSeconds;startedUtc=$startedUtc.ToString('o');startedQpc=$startedQpc;
        activeTrialPath=$activeTrialPath;applicationRunId=$null;applicationWallAnchorMs=$null;notification=$null;
        stoppedBy='startup-failure';components=$components;commands=$commands;failureReasons=$failureReasons}
    $statePath=Join-Path $root 'capture-state.json'
    $state | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $statePath -Encoding utf8
    & (Join-Path $PSScriptRoot 'startup_ingress_capture_stop.ps1') -StatePath $statePath
    exit 2
}

# Wait for an application burst notification newer than this capture start;
# notifications earlier than the start are ignored.
$notification=$null
$deadline=(Get-Date).AddSeconds($MaxWaitSeconds)
while((Get-Date) -lt $deadline){
    if($ChannelDir -and (Test-Path -LiteralPath $ChannelDir)){
        $candidates=Get-ChildItem -LiteralPath $ChannelDir -Filter 'burst-*.json' -File -ErrorAction SilentlyContinue
        foreach($candidate in $candidates){
            try{
                $note=Get-Content -Raw -LiteralPath $candidate.FullName | ConvertFrom-Json
                if($note.kind -eq 'burst' -and $note.trialId -eq $TrialId -and [int64]$note.notifiedWallMs -ge $startedUtcMs){
                    $notification=$note
                    break
                }
            } catch { }
        }
    }
    if($notification){ break }
    Start-Sleep -Milliseconds 200
}

$stoppedBy='timeout'
if($notification){ $stoppedBy='application-burst-notification' }

$state=[ordered]@{
    trialId=$TrialId
    outputDirectory=$root
    pktmonStarted=$true
    wprStarted=$wprStarted
    filterNames=$filterNames
    ports=$Ports
    cardIPs=$CardIPs
    wprProfile=$WprProfile
    maxWaitSeconds=$MaxWaitSeconds
    activeTrialPath=$activeTrialPath
    startedUtc=$startedUtc.ToString('o')
    startedQpc=$startedQpc
    applicationRunId=if($notification){ $notification.runId } else { $null }
    applicationWallAnchorMs=if($notification){ $notification.notifiedWallMs } else { $null }
    notification=$notification
    stoppedBy=$stoppedBy
    components=$components
    commands=$commands
    failureReasons=$failureReasons
}
$statePath=Join-Path $root 'capture-state.json'
$state | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $statePath -Encoding utf8

& (Join-Path $PSScriptRoot 'startup_ingress_capture_stop.ps1') -StatePath $statePath
exit $LASTEXITCODE
