param(
    [Parameter(Mandatory=$true)][string]$StatePath,
    [string]$AdapterPath = '',
    [switch]$StartFailure
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'system_capture_common.ps1')

$stateFile = Get-FullPathSafe $StatePath
$state = Read-CaptureJson $stateFile
if ($null -eq $state) {
    Write-Error 'capture-state.json is missing or unreadable'
    exit 2
}

$root = Get-FullPathSafe ([string]$state.outputDirectory)
if ([string]::IsNullOrWhiteSpace($root) -or -not (Test-Path -LiteralPath $root)) {
    Write-Error 'capture output directory is missing'
    exit 2
}

$lock = $null
$commands = [System.Collections.Generic.List[object]]::new()
$failures = [System.Collections.Generic.List[string]]::new()
$lifecycle = [System.Collections.Generic.List[object]]::new()
$conversion = [System.Collections.Generic.List[object]]::new()
$hadPktmon = $false
$hadWpr = $false
$ownerVerified = $false
$pktmonStopped = $false
$wprStopped = $false
$cleanupVerified = $false
$finalCode = 2

if ($state.PSObject.Properties.Name -contains 'commands') {
    foreach ($item in @($state.commands)) { if ($null -ne $item) { [void]$commands.Add($item) } }
}
if ($state.PSObject.Properties.Name -contains 'failureReasons') {
    foreach ($item in @($state.failureReasons)) { if (-not [string]::IsNullOrWhiteSpace([string]$item)) { [void]$failures.Add([string]$item) } }
}
if ($state.PSObject.Properties.Name -contains 'lifecycle') {
    foreach ($item in @($state.lifecycle)) { if ($null -ne $item) { [void]$lifecycle.Add($item) } }
}
if ($state.PSObject.Properties.Name -contains 'resources' -and $null -ne $state.resources) {
    $hadPktmon = [bool]$state.resources.pktmonStarted
    $hadWpr = [bool]$state.resources.wprStarted
    $ownerVerified = [bool]$state.resources.ownerVerified
}

function Add-StopFailure([string]$Message) {
    if (-not [string]::IsNullOrWhiteSpace($Message) -and -not $failures.Contains($Message)) { [void]$failures.Add($Message) }
}

function Save-StopState([string]$Phase, [string]$Reason = '') {
    $state.status = $Phase
    $state.phase = $Phase
    $state.updatedUtc = (Get-CaptureUtcNow).ToString('o')
    $state.updatedQpc = Get-CaptureQpc
    if (-not [string]::IsNullOrWhiteSpace($Reason)) { Add-StopFailure $Reason }
    [void]$lifecycle.Add([ordered]@{phase=$Phase;utc=$state.updatedUtc;qpc=$state.updatedQpc;reason=$Reason})
    $state.commands = @($commands)
    $state.failureReasons = @($failures)
    $state.lifecycle = @($lifecycle)
    Write-CaptureAtomicJson $stateFile $state 40
}

function Set-StopProperty([string]$Name, $Value) {
    if ($state.PSObject.Properties.Name -contains $Name) {
        $state.$Name = $Value
    } else {
        $state | Add-Member -NotePropertyName $Name -NotePropertyValue $Value
    }
}

function Save-StopStateOnly {
    $state.updatedUtc = (Get-CaptureUtcNow).ToString('o')
    $state.updatedQpc = Get-CaptureQpc
    $state.commands = @($commands)
    $state.failureReasons = @($failures)
    $state.lifecycle = @($lifecycle)
    Write-CaptureAtomicJson $stateFile $state 40
}

function Invoke-StopCommand {
    param(
        [Parameter(Mandatory=$true)][string]$Program,
        [Parameter(Mandatory=$true)][string[]]$Arguments,
        [Parameter(Mandatory=$true)][string]$Description,
        [int]$TimeoutSeconds = 60
    )
    $record = Invoke-CaptureProcess $Program $Arguments $Description $TimeoutSeconds $AdapterPath
    [void]$commands.Add($record)
    Save-StopStateOnly
    return $record
}

function Test-CommandSucceeded($Record) {
    return $null -ne $Record -and -not [bool]$Record.timedOut -and [int]$Record.exitCode -eq 0
}

function Get-ExistingFileRecord([string]$Path, [string]$Kind) {
    return Get-CaptureFileRecord $root $Path $Kind
}

function Add-FileIfPresent([System.Collections.Generic.List[object]]$List, [string]$Path, [string]$Kind) {
    if (Test-Path -LiteralPath $Path) { [void]$List.Add((Get-ExistingFileRecord $Path $Kind)) }
}

function Get-RequiredProperty($Object, [string]$Name, $Default = $null) {
    if ($null -ne $Object -and $Object.PSObject.Properties.Name -contains $Name) { return $Object.$Name }
    return $Default
}

try {
    if ($state.status -in @('complete','no_target_packets') -and (Test-Path -LiteralPath (Join-Path $root 'system-capture-manifest.json'))) {
        $finalCode = if ($state.status -eq 'complete') { 0 } else { 3 }
        exit $finalCode
    }
    $lock = New-CaptureLock $root
    if ($null -eq $lock) {
        Add-StopFailure 'capture session is already being stopped or started'
        Save-StopState 'failed'
        exit 2
    }

    Set-StopProperty 'stopStartedUtc' ((Get-CaptureUtcNow).ToString('o'))
    Set-StopProperty 'stopStartedQpc' (Get-CaptureQpc)
    Save-StopState 'stopping'

    $stateResources = if ($state.PSObject.Properties.Name -contains 'resources') { $state.resources } else { [pscustomobject]@{} }
    $stateFilterNames = @()
    if ($stateResources.PSObject.Properties.Name -contains 'filterNames') { $stateFilterNames = @($stateResources.filterNames | ForEach-Object { [string]$_ }) }
    $cardIPs = @((Get-RequiredProperty $state 'cardIPs' @()) | ForEach-Object { [string]$_ })
    $ports = @((Get-RequiredProperty $state 'ports' @()) | ForEach-Object { [int]$_ })
    $expectedFilters = if ($cardIPs.Count -gt 0 -and $ports.Count -gt 0) { @(New-CaptureFilterSpecs $cardIPs $ports) } else { @() }

    $pktmonStatusRecord = Invoke-StopCommand 'pktmon.exe' @('status') 'check pktmon ownership before stop' 15
    $pktmonState = Get-ToolState $pktmonStatusRecord 'pktmon'
    Set-StopProperty 'stopPktmonStatus' $pktmonState
    if ($hadPktmon -and $ownerVerified) {
        if ($pktmonState -eq 'running') {
            $pktmonStopRecord = Invoke-StopCommand 'pktmon.exe' @('stop') 'stop owned pktmon session' 60
            if (Test-CommandSucceeded $pktmonStopRecord) { $pktmonStopped = $true } else { Add-StopFailure 'pktmon stop command failed' }
        } elseif ($pktmonState -eq 'stopped') {
            $pktmonStopped = $true
        } elseif ($pktmonState -eq 'unknown' -or $pktmonState -eq 'command_failed') {
            Add-StopFailure ('pktmon stop ownership_unknown: ' + $pktmonState)
        }
    } elseif ($hadPktmon) {
        Add-StopFailure 'pktmon ownership was not verified; no global stop was attempted'
    }

    if ($hadWpr -and $ownerVerified) {
        $wprStatusRecord = Invoke-StopCommand 'wpr.exe' @('-status') 'check WPR ownership before stop' 15
        $wprState = Get-ToolState $wprStatusRecord 'wpr'
        Set-StopProperty 'stopWprStatus' $wprState
        if ($wprState -eq 'running') {
            $wprOutput = Join-Path $root 'wpr.etl'
            $wprStopRecord = Invoke-StopCommand 'wpr.exe' @('-stop',$wprOutput) 'stop owned WPR session and save ETL' 60
            if (Test-CommandSucceeded $wprStopRecord) { $wprStopped = $true } else { Add-StopFailure 'WPR stop command failed' }
        } elseif ($wprState -eq 'stopped') {
            $wprStopped = $true
        } else {
            Add-StopFailure ('WPR stop ownership_unknown: ' + $wprState)
        }
    }

    $pktmonEtl = Join-Path $root 'pktmon.etl'
    $pktmonPcap = Join-Path $root 'pktmon.pcapng'
    $pktmonText = Join-Path $root 'pktmon.txt'
    $pktmonDrop = Join-Path $root 'pktmon-drop.txt'
    if (Test-Path -LiteralPath $pktmonEtl) {
        $conversion.Add((Invoke-StopCommand 'pktmon.exe' @('etl2txt',$pktmonEtl,'-o',$pktmonText) 'convert pktmon ETL to text' 120))
        $conversion.Add((Invoke-StopCommand 'pktmon.exe' @('etl2txt',$pktmonEtl,'-o',$pktmonDrop,'--drop-only') 'convert pktmon drop-only evidence' 120))
        $conversion.Add((Invoke-StopCommand 'pktmon.exe' @('etl2pcap',$pktmonEtl,'-o',$pktmonPcap) 'convert pktmon ETL to PcapNG' 120))
    } else {
        Add-StopFailure 'pktmon ETL is missing after stop'
    }

    Save-StopState 'validating'

    $filterCleanupStatus = 'not_attempted'
    $filterListRecord = Invoke-StopCommand 'pktmon.exe' @('filter','list') 'verify pktmon filters before cleanup' 15
    $filterInventory = Get-FilterInventory $filterListRecord
    Set-StopProperty 'stopFilterInventoryBeforeCleanup' $filterInventory
    if (Test-FilterInventoryEmpty $filterInventory) {
        $cleanupVerified = $true
        $filterCleanupStatus = 'already_empty'
    } elseif ($ownerVerified -and $expectedFilters.Count -gt 0 -and (Test-FilterInventoryExact $filterInventory $expectedFilters)) {
        $removeHelp = Invoke-StopCommand 'pktmon.exe' @('filter','remove','help') 'check named pktmon filter removal support' 15
        $namedRemoval = Test-CommandSucceeded $removeHelp
        if ($namedRemoval) {
            foreach ($filterName in @($stateFilterNames)) {
                $removeRecord = Invoke-StopCommand 'pktmon.exe' @('filter','remove',$filterName) ('remove owned pktmon filter ' + $filterName) 15
                if (-not (Test-CommandSucceeded $removeRecord)) { $namedRemoval = $false; Add-StopFailure ('named filter removal failed: ' + $filterName) }
            }
        }
        if (-not $namedRemoval) {
            $wholeRemove = Invoke-StopCommand 'pktmon.exe' @('filter','remove') 'remove exact owned pktmon filter set' 15
            if (-not (Test-CommandSucceeded $wholeRemove)) { Add-StopFailure 'owned pktmon filter cleanup failed' }
        }
        $afterCleanup = Invoke-StopCommand 'pktmon.exe' @('filter','list') 'verify pktmon filters after cleanup' 15
        Set-StopProperty 'stopFilterInventoryAfterCleanup' (Get-FilterInventory $afterCleanup)
        if (Test-FilterInventoryEmpty $state.stopFilterInventoryAfterCleanup) {
            $cleanupVerified = $true
            $filterCleanupStatus = 'empty_after_owned_cleanup'
        } else {
            $filterCleanupStatus = 'cleanup_incomplete'
            Add-StopFailure 'pktmon filters remain after owned cleanup'
        }
    } else {
        $filterCleanupStatus = 'ownership_unknown'
        Add-StopFailure 'pktmon filter cleanup skipped because the actual filter set was not exactly owned'
    }

    $pcapValidation = Test-CapturePcapNg $pktmonPcap $cardIPs $ports
    $fileRecords = [System.Collections.Generic.List[object]]::new()
    Add-FileIfPresent $fileRecords $pktmonEtl 'pktmon_etl'
    Add-FileIfPresent $fileRecords $pktmonPcap 'pktmon_pcapng'
    Add-FileIfPresent $fileRecords $pktmonText 'pktmon_text'
    Add-FileIfPresent $fileRecords $pktmonDrop 'pktmon_drop_only'
    $wprEtl = Join-Path $root 'wpr.etl'
    if (-not [bool](Get-RequiredProperty $state 'noWpr' $false)) { Add-FileIfPresent $fileRecords $wprEtl 'wpr_etl' }

    $pktmonEtlRecord = Get-ExistingFileRecord $pktmonEtl 'pktmon_etl'
    $pktmonPcapRecord = Get-ExistingFileRecord $pktmonPcap 'pktmon_pcapng'
    $dropRecord = Get-ExistingFileRecord $pktmonDrop 'pktmon_drop_only'
    $wprRecord = if ([bool](Get-RequiredProperty $state 'noWpr' $false)) { [ordered]@{kind='wpr_etl';relativePath='';bytes=0;sha256=$null;missing=$false;skipped='NoWpr'} } else { Get-ExistingFileRecord $wprEtl 'wpr_etl' }

    $pktmonCommandsSuccess = @($commands | Where-Object { $_.description -match 'pktmon' -and $_.description -notmatch 'ownership' } | Where-Object { -not (Test-CommandSucceeded $_) }).Count -eq 0
    $conversionSuccess = @($conversion | Where-Object { -not (Test-CommandSucceeded $_) }).Count -eq 0
    $artifactValid = (-not $pktmonEtlRecord.missing -and -not $pktmonPcapRecord.missing -and [bool]$pcapValidation.parseable -and $conversionSuccess)
    $wprSkipped = [bool](Get-RequiredProperty $state 'noWpr' $false)
    $wprArtifactValid = $wprSkipped -or (-not $wprRecord.missing -and [int64]$wprRecord.bytes -gt 0)
    if (-not $wprArtifactValid) { Add-StopFailure 'WPR ETL is missing or empty' }
    $commandSuccess = ($failures.Count -eq 0 -and $pktmonCommandsSuccess -and $conversionSuccess -and $pktmonStopped -and ($wprSkipped -or $wprStopped) -and $cleanupVerified)
    $targetPacketsPresent = $pcapValidation.targetPacketsPresent
    $timeCoverage = if ($pcapValidation.parseable -and $null -ne $pcapValidation.firstTimestampNs -and $null -ne $pcapValidation.lastTimestampNs) { 'known' } else { 'unknown' }
    $lossStatus = if (Test-Path -LiteralPath $pktmonDrop) { 'drop_info_available_etw_loss_unknown' } else { 'unknown' }
    $layerCoverage = if ($wprSkipped) { 'pktmon_only_wpr_skipped' } elseif ($wprArtifactValid) { 'pktmon_plus_wpr' } else { 'pktmon_only_wpr_unknown' }
    $analysisReady = ($commandSuccess -and $artifactValid -and [bool]$targetPacketsPresent -and $timeCoverage -ne 'unknown' -and $layerCoverage -ne 'pktmon_only_wpr_unknown')

    $stopUtc = Get-CaptureUtcNow
    $stopQpc = Get-CaptureQpc
    $clockAnchors = [ordered]@{
        schemaVersion = 1
        utcNow = $stopUtc.ToString('o')
        qpcNow = $stopQpc
        qpcFrequency = Get-CaptureQpcFrequency
        ready = Get-RequiredProperty $state 'ready' $null
        stopStartedUtc = Get-RequiredProperty $state 'stopStartedUtc' $null
        stopStartedQpc = Get-RequiredProperty $state 'stopStartedQpc' $null
        stopUtc = $stopUtc.ToString('o')
        stopQpc = $stopQpc
        firstPacketTimestampNs = $pcapValidation.firstTimestampNs
        lastPacketTimestampNs = $pcapValidation.lastTimestampNs
    }
    $clockPath = Join-Path $root 'clock-anchors.json'
    Write-CaptureAtomicJson $clockPath $clockAnchors 40

    $validationSummary = [ordered]@{
        schemaVersion = 2
        status = 'validated'
        pcap = $pcapValidation
        commandSuccess = $commandSuccess
        artifactValid = $artifactValid
        targetPacketsPresent = $targetPacketsPresent
        timeCoverage = $timeCoverage
        lossStatus = $lossStatus
        layerCoverage = $layerCoverage
        pktmonStopConfirmed = $pktmonStopped
        wprStopConfirmed = ($wprSkipped -or $wprStopped)
        filterCleanup = $filterCleanupStatus
        analysisReady = $analysisReady
        failures = @($failures)
    }
    $validationPath = Join-Path $root 'validation-summary.json'
    Write-CaptureAtomicJson $validationPath $validationSummary 40

    Add-FileIfPresent $fileRecords $clockPath 'clock_anchors'
    Add-FileIfPresent $fileRecords $validationPath 'validation_summary'
    $fileRecordsUnique = @($fileRecords | Group-Object relativePath | ForEach-Object { $_.Group[0] })
    $manifestStatus = if (-not $artifactValid -or $failures.Count -gt 0) { if ($pktmonEtlRecord.missing -and $pktmonPcapRecord.missing) { 'failed' } else { 'partial' } } elseif (-not [bool]$targetPacketsPresent) { 'no_target_packets' } else { 'complete' }
    $manifest = [ordered]@{
        schemaVersion = 3
        status = $manifestStatus
        trialId = Get-RequiredProperty $state 'trialId' ''
        captureSessionToken = Get-RequiredProperty $state 'captureSessionToken' ''
        runId = Get-RequiredProperty $state 'runId' ''
        listenId = Get-RequiredProperty $state 'listenId' ''
        measurementSessionId = Get-RequiredProperty $state 'measurementSessionId' ''
        outputDirectory = $root
        simulation = [bool](Get-RequiredProperty $state 'simulation' $false)
        captureMode = Get-RequiredProperty $state 'captureMode' 'pktmon+wpr'
        commandSuccess = $commandSuccess
        artifactValid = $artifactValid
        targetPacketsPresent = $targetPacketsPresent
        timeCoverage = $timeCoverage
        lossStatus = $lossStatus
        layerCoverage = $layerCoverage
        analysisReady = $analysisReady
        ownership = [ordered]@{verified=$ownerVerified;pktmonStopped=$pktmonStopped;wprStopped=($wprSkipped -or $wprStopped);filterCleanup=$filterCleanupStatus}
        validation = $validationSummary
        clockAnchors = 'clock-anchors.json'
        files = $fileRecordsUnique
        commands = @($commands)
        createdUtc = Get-RequiredProperty $state 'createdUtc' ''
        stoppedUtc = $stopUtc.ToString('o')
        stoppedQpc = $stopQpc
    }
    $manifestPath = Join-Path $root 'system-capture-manifest.json'
    Write-CaptureAtomicJson $manifestPath $manifest 60

    Set-StopProperty 'manifestPath' $manifestPath
    Set-StopProperty 'validationPath' $validationPath
    Set-StopProperty 'clockAnchorsPath' $clockPath
    Set-StopProperty 'files' $fileRecordsUnique
    Set-StopProperty 'commandSuccess' $commandSuccess
    Set-StopProperty 'artifactValid' $artifactValid
    Set-StopProperty 'targetPacketsPresent' $targetPacketsPresent
    Set-StopProperty 'timeCoverage' $timeCoverage
    Set-StopProperty 'lossStatus' $lossStatus
    Set-StopProperty 'layerCoverage' $layerCoverage
    Set-StopProperty 'analysisReady' $analysisReady
    $state.resources.pktmonStarted = $false
    $state.resources.wprStarted = $false
    $state.resources.filterNames = @()
    $state.resources.ownerVerified = $ownerVerified
    Set-StopProperty 'completedUtc' ($stopUtc.ToString('o'))
    Set-StopProperty 'completedQpc' $stopQpc
    Save-StopState $manifestStatus
    Write-CaptureAtomicJson (Join-Path $root 'capture-complete.json') ([ordered]@{schemaVersion=1;status=$manifestStatus;trialId=$manifest.trialId;captureSessionToken=$manifest.captureSessionToken;manifest='system-capture-manifest.json';completedUtc=$stopUtc.ToString('o');analysisReady=$analysisReady})

    $finalCode = switch ($manifestStatus) { 'complete' { 0 } 'no_target_packets' { 3 } 'partial' { 2 } default { 2 } }
    exit $finalCode
} catch {
    $message = 'stop script exception: ' + $_.Exception.Message
    Add-StopFailure $message
    try {
        $state.commands = @($commands)
        $state.failureReasons = @($failures)
        Save-StopState 'failed'
    } catch {}
    Write-Error $message
    exit 2
} finally {
    Release-CaptureLock $lock
}
