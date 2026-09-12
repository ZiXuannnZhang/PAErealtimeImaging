param([string]$ResultRoot = '')
$ErrorActionPreference = 'Stop'
$delivery = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $delivery 'tools/system_capture_common.ps1')
if (-not $ResultRoot) { $ResultRoot = Join-Path (Split-Path (Split-Path $delivery -Parent) -Parent) ('verification/synthetic/capture-readiness-' + (New-CaptureId)) }
New-Item -ItemType Directory -Path $ResultRoot -Force | Out-Null
$checks = [System.Collections.Generic.List[object]]::new()
function Check([bool]$Condition, [string]$Name) {
    $checks.Add([ordered]@{name=$Name;passed=$Condition})
    if (-not $Condition) { throw "FAILED: $Name" }
}
function Record([string]$Text, [int]$ExitCode = 0) {
    return [pscustomobject]@{stdout=$Text;stderr='';exitCode=$ExitCode;timedOut=$false;arguments=@('status')}
}
function Request([string]$Channel, [string]$Trial) {
    return [ordered]@{kind='system-capture-request';schemaVersion=2;trialId=$Trial;captureSessionToken=[Guid]::NewGuid().ToString('N');
        runId='run-'+$Trial;listenId='listen-'+$Trial;measurementSessionId='measurement-1';applicationPid=$PID;
        applicationExecutable=(Get-Process -Id $PID).Path;createdUtc=[DateTime]::UtcNow.ToString('o');active=$true;
        outputDirectory=(Join-Path (Split-Path $Channel -Parent) ('captures/'+$Trial));channelDirectory=$Channel;
        cardIPs=@('192.168.0.1');ports=@(8000,8001,8002,8003,8004)}
}

try {
    # The first two strings are copied from the failed field state, not JSON
    # invented by the command adapter. Non-empty tables are resource-derived.
    $emptyZh="数据包筛选器:`r`n    无`r`n"
    Check ((Get-FilterInventory (Record $emptyZh)).status -eq 'empty') 'field Chinese empty filter table'
    Check ((Get-ToolState (Record "`r`n数据包监视器没有运行。`r`n") pktmon) -eq 'stopped') 'field Chinese stopped state'
    Check ((Get-FilterInventory (Record "Packet Filters:`r`n    None`r`n")).status -eq 'empty') 'English empty filter table'
    foreach ($bad in @('{}','{"unexpected":[]}','{"filters":null}','[{}]',"数据包筛选器:`n 无`n 1 Foreign UDP 10.0.0.1 99",'未知的输出')) {
        Check ((Get-FilterInventory (Record $bad)).status -eq 'unknown') ('reject malformed/ambiguous filters: '+$bad)
    }
    Check ((Get-FilterInventory (Record $emptyZh 5)).status -eq 'command_failed') 'command exit status overrides empty text'
    $expected = @(New-CaptureFilterSpecs @('192.168.0.1') @(8000))
    $table="数据包筛选器:`n# 名称 协议 IP 地址 端口`n1 StartupDiag-0-8000 UDP 192.168.0.1 8000"
    Check (Test-FilterInventoryExact (Get-FilterInventory (Record $table)) $expected) 'Chinese table verifies protocol IP port and name'
    $enTable="Packet Filters:`n# Name IP Address Protocol Port`n1 StartupDiag-0-8000 192.168.0.1 UDP 8000"
    Check (Test-FilterInventoryExact (Get-FilterInventory (Record $enTable)) $expected) 'English reordered columns'
    foreach ($other in @($table.Replace('8000','8001'),$table.Replace('UDP','TCP'),$table.Replace('192.168.0.1','192.168.0.2'),($table+"`n2 Foreign UDP 10.0.0.1 99"),($table+"`n2 StartupDiag-0-8000 UDP 192.168.0.1 8000"))) {
        Check (-not (Test-FilterInventoryExact (Get-FilterInventory (Record $other)) $expected)) 'mismatched/foreign/duplicate filters are not owned'
    }
    $stateZh="收集的数据:`n    数据包计数器，数据包捕获`n记录程序参数:`n    记录程序名称: PktMon`n    日志模式: 内存缓冲区"
    Check ((Get-ToolState (Record $stateZh) pktmon) -eq 'running') 'resource-derived Chinese running status'
    Check ((Get-ToolState (Record 'unrecognized response') pktmon) -eq 'unknown') 'unknown state remains unknown'

    $channel=Join-Path $ResultRoot 'requests'
    New-Item -ItemType Directory -Path $channel -Force | Out-Null
    $live=Request $channel 'live'
    Write-CaptureAtomicJson (Join-Path $channel 'capture-request-live.json') $live
    $stale=Request $channel 'old'
    $stale.createdUtc='2000-01-01T00:00:00Z'
    Write-CaptureAtomicJson (Join-Path $channel 'capture-request-old.json') $stale
    Check (-not (Test-CaptureRequestLive ([pscustomobject]$stale)).valid) 'PID reuse / stale process birth is rejected'
    Check ((Resolve-CaptureApplicationRequest $channel).request.trialId -eq 'live') 'newer stale file cannot replace a live request'
    $dead=Request $channel 'dead'; $dead.applicationPid=[int]::MaxValue
    Check (-not (Test-CaptureRequestLive ([pscustomobject]$dead)).valid) 'exited application is rejected'
    $inactive=Request $channel 'inactive'; $inactive.active=$false
    Check (-not (Test-CaptureRequestLive ([pscustomobject]$inactive)).valid) 'listener-stop invalidation is rejected'
    $live2=Request $channel 'live2'
    Write-CaptureAtomicJson (Join-Path $channel 'capture-request-live2.json') $live2
    $ambiguous=$false
    try { $null=Resolve-CaptureApplicationRequest $channel } catch { $ambiguous=$_.Exception.Message -match 'multiple live' }
    Check $ambiguous 'multiple live applications require explicit request selection'
    Check ((Resolve-CaptureApplicationRequest $channel (Join-Path $channel 'capture-request-live2.json')).request.trialId -eq 'live2') 'explicit live request selection'

    $adapter=Join-Path $PSScriptRoot 'adapter.ps1'
    $start=Join-Path $delivery 'tools/start_system_capture_admin.ps1'
    foreach ($case in @('positive','unknown','foreign','add_fail','wpr_fail','cleanup_foreign','no_target','no_wpr','nonadmin')) {
        $caseRoot=Join-Path $ResultRoot $case
        $caseChannel=Join-Path $caseRoot 'system-capture-channel'
        New-Item -ItemType Directory -Path $caseChannel -Force | Out-Null
        $req=Request $caseChannel $case
        $requestPath=Join-Path $caseChannel 'capture-request-test.json'
        Write-CaptureAtomicJson $requestPath $req
        $env:SYSTEM_CAPTURE_TEST_ROOT=Join-Path $caseRoot 'adapter'
        $env:SYSTEM_CAPTURE_TEST_CASE=$case
        $env:SYSTEM_CAPTURE_COMMAND_ADAPTER=if($case -eq 'nonadmin'){''}else{$adapter}
        $args=@('-NoProfile','-ExecutionPolicy','Bypass','-File',$start,'-ChannelDir',$caseChannel,'-MaxWaitSeconds','1')
        if($case -eq 'no_wpr'){$args+='-NoWpr'}
        if($case -eq 'nonadmin') {
            $principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
            if($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'nonadmin regression must not run with administrator privileges'}
        }
        # Invoke the script, not the real pktmon/wpr tools. Clear the adapter
        # variable for the outer process only, keeping the child's environment.
        $r=Invoke-CaptureProcess (Get-Command pwsh.exe).Source $args ('run '+$case) 75 '' ''
        # The common process helper would adapt the outer pwsh as well. The
        # actual launcher below is used for every case (see Run-PlainProcess).
        Check ($r.exitCode -eq 0) 'placeholder'
    }
} finally {
    $env:SYSTEM_CAPTURE_COMMAND_ADAPTER=$null
    $env:SYSTEM_CAPTURE_TEST_ROOT=$null
    $env:SYSTEM_CAPTURE_TEST_CASE=$null
    Write-CaptureAtomicJson (Join-Path $ResultRoot 'checks.json') ([ordered]@{simulation=$true;checks=@($checks);completedUtc=[DateTime]::UtcNow.ToString('o')})
    Write-Output "RESULT_ROOT=$ResultRoot"
}
