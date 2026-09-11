param(
    [Parameter(Mandatory=$true)][string]$StatePath
)
$ErrorActionPreference='Stop'

# Manual stop path for the startup ingress system capture. Reads the state
# file written by startup_ingress_capture.ps1, stops only the sessions this
# task created, converts results and completes the manifest. Any failure is
# recorded; a failed capture never reports success with empty output.

function Invoke-Tool([string]$Program,[string[]]$ToolArgs,[string]$Description){
    $begin=[DateTime]::UtcNow
    $text=& $Program @ToolArgs 2>&1 | Out-String
    $exit=$LASTEXITCODE
    return [ordered]@{description=$Description;command=("$Program "+($ToolArgs -join ' '));
        exitCode=$exit;startUtc=$begin.ToString('o');endUtc=[DateTime]::UtcNow.ToString('o');
        output=$text}
}
function Add-File([System.Collections.Generic.List[object]]$Files,[string]$Path,[string]$Kind){
    if(Test-Path -LiteralPath $Path){
        $item=Get-Item -LiteralPath $Path
        $hash=Get-FileHash -LiteralPath $Path -Algorithm SHA256
        $Files.Add([ordered]@{kind=$Kind;path=$item.FullName;bytes=$item.Length;sha256=$hash.Hash})
    } else {
        $Files.Add([ordered]@{kind=$Kind;path=$Path;bytes=0;sha256=$null;missing=$true})
    }
}

$state=Get-Content -Raw -LiteralPath $StatePath | ConvertFrom-Json
$root=$state.outputDirectory
$commands=[System.Collections.Generic.List[object]]::new()
if($state.commands){ foreach($c in $state.commands){ $commands.Add($c) } }
$files=[System.Collections.Generic.List[object]]::new()
if($state.files){ foreach($f in $state.files){ $files.Add($f) } }

$stopBeginWall=[DateTime]::UtcNow
$stopBeginQpc=[System.Diagnostics.Stopwatch]::GetTimestamp()
$failed=$false
$failureReasons=[System.Collections.Generic.List[string]]::new()
if($state.failureReasons){ foreach($reason in $state.failureReasons){ $failureReasons.Add([string]$reason); $failed=$true } }

if($state.pktmonStarted){
    $pktmonStop=Invoke-Tool 'pktmon.exe' @('stop') 'stop pktmon session created by this task'
    $commands.Add($pktmonStop)
    if($pktmonStop.exitCode -ne 0){ $failed=$true; $failureReasons.Add('pktmon stop failed') }
    $etl=Join-Path $root 'startup-pktmon.etl'
    $etlItem=Get-Item -LiteralPath $etl -ErrorAction SilentlyContinue
    if(-not $etlItem -or $etlItem.Length -eq 0){ $failed=$true; $failureReasons.Add('pktmon ETL missing or empty') }
    Add-File $files $etl 'pktmon-etl'
    if($etlItem -and $etlItem.Length -gt 0){
        $pcap=Join-Path $root 'startup-pktmon.pcapng'
        $convert=Invoke-Tool 'pktmon.exe' @('etl2pcap',$etl,'--out',$pcap) 'convert pktmon ETL to pcapng'
        $commands.Add($convert)
        if($convert.exitCode -ne 0){ $failed=$true; $failureReasons.Add('pktmon pcapng conversion failed') }
        Add-File $files $pcap 'pktmon-pcapng'
        $drops=Join-Path $root 'startup-pktmon-drops.pcapng'
        $dropConvert=Invoke-Tool 'pktmon.exe' @('etl2pcap',$etl,'--drop-only','--out',$drops) 'convert pktmon drop events to pcapng'
        $commands.Add($dropConvert)
        if($dropConvert.exitCode -ne 0){ $failed=$true; $failureReasons.Add('pktmon drop conversion failed') }
        Add-File $files $drops 'pktmon-drop-pcapng'
    }
}
if($state.wprStarted){
    $wprEtl=Join-Path $root 'startup-wpr.etl'
    $wprStop=Invoke-Tool 'wpr.exe' @('-stop',$wprEtl) 'stop WPR session created by this task'
    $commands.Add($wprStop)
    if($wprStop.exitCode -ne 0){ $failed=$true; $failureReasons.Add('WPR stop failed') }
    $wprItem=Get-Item -LiteralPath $wprEtl -ErrorAction SilentlyContinue
    if(-not $wprItem -or $wprItem.Length -eq 0){ $failed=$true; $failureReasons.Add('WPR ETL missing or empty') }
    Add-File $files $wprEtl 'wpr-etl'
}

# This Windows pktmon version can only remove all filters. The start script
# proved the filter set was empty before adding ours, so all active filters at
# this point are the set recorded in this state file.
if($state.pktmonStarted){
    $filterRemove=Invoke-Tool 'pktmon.exe' @('filter','remove') 'remove filter set created by this task'
    $commands.Add($filterRemove)
    if($filterRemove.exitCode -ne 0){ $failed=$true; $failureReasons.Add('pktmon filter cleanup failed') }
}

$stopEndQpc=[System.Diagnostics.Stopwatch]::GetTimestamp()
$stopEndWall=[DateTime]::UtcNow
$frequency=[System.Diagnostics.Stopwatch]::Frequency

$missingOutputs=@($files | Where-Object { $_.missing -or $_.bytes -eq 0 })
if($missingOutputs.Count){ $failed=$true; $failureReasons.Add('one or more capture outputs missing or empty') }

$manifest=[ordered]@{
    schemaVersion=1
    purpose='startup ingress system capture (Pktmon + WPR)'
    trialId=$state.trialId
    applicationRunId=$state.applicationRunId
    applicationNotification=$state.notification
    configuration=[ordered]@{
        ports=@($state.ports)
        cardIPs=@($state.cardIPs)
        pktmonPacketTruncationBytes=128
        pktmonMemoryModeMB=256
        pktmonType='all (flow and drop events)'
        pktmonComponents='all'
        wprProfile=$state.wprProfile
        wprLoggingMode='memory'
        maxWaitSeconds=$state.maxWaitSeconds
    }
    filters=@($state.filterNames)
    components=$state.components
    clockAnchors=[ordered]@{
        qpcFrequency=$frequency
        scriptStartUtc=$state.startedUtc
        scriptStartQpc=$state.startedQpc
        scriptEndUtc=$stopEndWall.ToString('o')
        scriptEndQpc=$stopEndQpc
        applicationMonotonicAssumedQpcBased='unknown; steady_clock on MSVC/MinGW uses QPC but alignment error is not verified here'
        applicationWallAnchorMs=$state.applicationWallAnchorMs
        applicationBurstMonotonicNs=if($state.notification){ $state.notification.burstMonotonicNs } else { $null }
        applicationNotificationMonotonicNs=if($state.notification){ $state.notification.notifiedMonotonicNs } else { $null }
    }
    etwDroppedEvents='unknown; pktmon stop output retained per command'
    circularBufferCoverage='unknown; manifest records tool output only and does not prove completeness'
    observableLayerCoverage='unknown; pcapng conversion success does not prove every layer was observable'
    commands=$commands
    files=$files
    stopInterval=[ordered]@{
        startUtc=$stopBeginWall.ToString('o')
        endUtc=$stopEndWall.ToString('o')
        note='stopping and ETL flush perform file I/O that can affect the remaining acquisition'
    }
    stoppedBy=$state.stoppedBy
    status=''
    failureReasons=@($failureReasons)
}

if($failed){
    $manifest.status='failed'
} elseif($state.stoppedBy -eq 'application-burst-notification'){
    $manifest.status='captured-burst'
} else {
    $manifest.status='captured-timeout'
}
$manifestPath=Join-Path $root 'system-capture-manifest.json'
$manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath -Encoding utf8
$manifest | ConvertTo-Json -Depth 4
if($failed){ exit 2 }
exit 0
