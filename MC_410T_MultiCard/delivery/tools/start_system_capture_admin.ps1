param(
    [string]$OutputDirectory = '',
    [string]$TrialId = '',
    [string]$ChannelDir = '',
    [string[]]$CardIPs = @(),
    [int[]]$Ports = @(8000,8001,8002,8003,8004),
    [string]$RunId = '',
    [string]$ListenId = '',
    [string]$MeasurementSessionId = '',
    [string]$CaptureSessionToken = '',
    [string]$ApplicationRequestPath = '',
    [string]$AdapterPath = '',
    [string]$WprProfile = 'GeneralProfile',
    [int]$MaxWaitSeconds = 60,
    [switch]$NoWpr
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'system_capture_common.ps1')

# When launched from Open-AdminCapture.cmd without arguments, consume the
# newest application request. Explicit parameters still win and are recorded
# in the state/command manifest.
$appBinRoot = Split-Path $PSScriptRoot -Parent
if (-not [string]::IsNullOrWhiteSpace($ApplicationRequestPath) -and (Test-Path -LiteralPath $ApplicationRequestPath)) {
    $initialRequest = Read-CaptureJson $ApplicationRequestPath
    if ($null -ne $initialRequest) {
        if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = [string]$initialRequest.outputDirectory }
        if ([string]::IsNullOrWhiteSpace($ChannelDir)) { $ChannelDir = [string]$initialRequest.channelDirectory }
    }
}
if ([string]::IsNullOrWhiteSpace($ChannelDir)) {
    $ChannelDir = Join-Path $appBinRoot 'system-capture-channel'
}
$ChannelDir = Get-FullPathSafe $ChannelDir
if ([string]::IsNullOrWhiteSpace($OutputDirectory) -and (Test-Path -LiteralPath $ChannelDir)) {
    $requestCandidates = @(Get-ChildItem -LiteralPath $ChannelDir -Filter 'capture-request*.json' -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending)
    foreach ($candidate in $requestCandidates) {
        $candidateValue = Read-CaptureJson $candidate.FullName
        if ($null -ne $candidateValue -and -not [string]::IsNullOrWhiteSpace([string]$candidateValue.outputDirectory)) {
            $OutputDirectory = [string]$candidateValue.outputDirectory
            if ([string]::IsNullOrWhiteSpace($ApplicationRequestPath)) { $ApplicationRequestPath = $candidate.FullName }
            break
        }
    }
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    throw 'OutputDirectory is required, or an application capture-request*.json must be available in the capture channel'
}
$root = Get-FullPathSafe $OutputDirectory
if ([string]::IsNullOrWhiteSpace($ChannelDir)) {
    $artifactRoot = Split-Path (Split-Path $root -Parent) -Parent
    $ChannelDir = Join-Path $artifactRoot 'system-capture-channel'
    $ChannelDir = Get-FullPathSafe $ChannelDir
}
if ([string]::IsNullOrWhiteSpace($TrialId)) {
    $trialCandidates = @()
    if (-not [string]::IsNullOrWhiteSpace($ApplicationRequestPath) -and (Test-Path -LiteralPath $ApplicationRequestPath)) {
        $trialCandidates = @(Get-Item -LiteralPath $ApplicationRequestPath)
    } elseif (Test-Path -LiteralPath $ChannelDir) {
        $trialCandidates = @(Get-ChildItem -LiteralPath $ChannelDir -Filter 'capture-request*.json' -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTimeUtc -Descending)
    }
    foreach ($candidate in $trialCandidates) {
        $candidateRequest = Read-CaptureJson $candidate.FullName
        if ($null -ne $candidateRequest -and -not [string]::IsNullOrWhiteSpace([string]$candidateRequest.trialId)) {
            $TrialId = [string]$candidateRequest.trialId
            break
        }
    }
    if ([string]::IsNullOrWhiteSpace($TrialId)) { $TrialId = New-CaptureId }
}
# The operator entry point normally has no explicit CaptureSessionToken. If a
# previous attempt already created the application request/state pair, adopt
# that request token before checking output-directory ownership. Otherwise a
# retry would manufacture a new token and reject its own resumable session.
$preflightRequest = $null
if (-not [string]::IsNullOrWhiteSpace($ApplicationRequestPath) -and (Test-Path -LiteralPath $ApplicationRequestPath)) {
    $preflightRequest = Read-CaptureJson $ApplicationRequestPath
} elseif (Test-Path -LiteralPath $ChannelDir) {
    $preflightCandidates = @(Get-ChildItem -LiteralPath $ChannelDir -Filter 'capture-request*.json' -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending)
    foreach ($candidate in $preflightCandidates) {
        $candidateRequest = Read-CaptureJson $candidate.FullName
        if ($null -eq $candidateRequest) { continue }
        $candidateTrialId = if ($candidateRequest.PSObject.Properties.Name -contains 'trialId') { [string]$candidateRequest.trialId } else { '' }
        $candidateOutput = if ($candidateRequest.PSObject.Properties.Name -contains 'outputDirectory') { [string]$candidateRequest.outputDirectory } else { '' }
        if (-not [string]::IsNullOrWhiteSpace($candidateTrialId) -and $candidateTrialId -ne $TrialId) { continue }
        if (-not [string]::IsNullOrWhiteSpace($candidateOutput) -and (Get-FullPathSafe $candidateOutput) -ne $root) { continue }
        $ApplicationRequestPath = $candidate.FullName
        $preflightRequest = $candidateRequest
        break
    }
}
if ($null -ne $preflightRequest -and [string]::IsNullOrWhiteSpace($CaptureSessionToken) -and
    $preflightRequest.PSObject.Properties.Name -contains 'captureSessionToken') {
    $preflightToken = [string]$preflightRequest.captureSessionToken
    if (-not [string]::IsNullOrWhiteSpace($preflightToken)) { $CaptureSessionToken = $preflightToken }
}
$statePath = Join-Path $root 'capture-state.json'
$stopScript = Join-Path $PSScriptRoot 'stop_system_capture_admin.ps1'
$sessionToken = if ([string]::IsNullOrWhiteSpace($CaptureSessionToken)) { [Guid]::NewGuid().ToString('N') } else { $CaptureSessionToken }
$adapterSimulation = -not [string]::IsNullOrWhiteSpace($AdapterPath)
$commands = [System.Collections.Generic.List[object]]::new()
$failures = [System.Collections.Generic.List[string]]::new()
$lifecycle = [System.Collections.Generic.List[object]]::new()
$filterNames = [System.Collections.Generic.List[string]]::new()
$pktmonStarted = $false
$wprStarted = $false
$lock = $null
$state = $null
$stopCode = 2
$resolvedApplicationRequestPath = ''
$explicitSessionToken = -not [string]::IsNullOrWhiteSpace($CaptureSessionToken)

function Save-State {
    param([string]$Phase, [string]$Reason = '')
    $state.status = $Phase
    $state.phase = $Phase
    $state.updatedUtc = (Get-CaptureUtcNow).ToString('o')
    $state.updatedQpc = Get-CaptureQpc
    if (-not [string]::IsNullOrWhiteSpace($Reason)) { Add-CaptureFailure $state.failureReasons $Reason }
    $lifecycle.Add([ordered]@{phase=$Phase;utc=$state.updatedUtc;qpc=$state.updatedQpc;reason=$Reason})
    Write-CaptureAtomicJson $statePath $state 40
}

function Save-StateWithoutTransition {
    $state.updatedUtc = (Get-CaptureUtcNow).ToString('o')
    $state.updatedQpc = Get-CaptureQpc
    Write-CaptureAtomicJson $statePath $state 40
}

function Add-Command {
    param($Record)
    $commands.Add($Record)
    $state.commands = $commands
    Save-StateWithoutTransition
    return $Record
}

function Fail-Start {
    param([string]$Reason)
    Add-CaptureFailure $failures $Reason
    $state.failureReasons = $failures
    Save-State 'failed'
}

function Read-ApplicationHandshake {
    if ([string]::IsNullOrWhiteSpace($ApplicationRequestPath)) {
        $candidates = @()
        if (Test-Path -LiteralPath $ChannelDir) {
            $candidates = @(Get-ChildItem -LiteralPath $ChannelDir -Filter 'capture-request*.json' -File -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTimeUtc -Descending)
        }
        foreach ($candidate in $candidates) {
            $value = Read-CaptureJson $candidate.FullName
            if ($null -ne $value -and ([string]$value.trialId -eq $TrialId -or [string]::IsNullOrWhiteSpace([string]$value.trialId))) {
                $script:resolvedApplicationRequestPath = $candidate.FullName
                break
            }
        }
    }
    if ([string]::IsNullOrWhiteSpace($ApplicationRequestPath) -and -not [string]::IsNullOrWhiteSpace($script:resolvedApplicationRequestPath)) {
        $ApplicationRequestPath = $script:resolvedApplicationRequestPath
    }
    if ([string]::IsNullOrWhiteSpace($ApplicationRequestPath)) { return $null }
    $script:resolvedApplicationRequestPath = $ApplicationRequestPath
    $request = Read-CaptureJson $ApplicationRequestPath
    if ($null -eq $request) { return $null }
    return $request
}

function Resolve-StringArray($Value) {
    if ($null -eq $Value) { return @() }
    if ($Value -is [string]) { return @([string]$Value) }
    return @($Value | ForEach-Object { [string]$_ })
}

function Stop-AfterStartFailure {
    Save-State 'failed'
    if ($pktmonStarted -or $wprStarted -or $filterNames.Count -gt 0) {
        if (Test-Path -LiteralPath $stopScript) {
            Release-CaptureLock $script:lock
            $script:lock = $null
            & $stopScript -StatePath $statePath -AdapterPath $AdapterPath -StartFailure | Out-Host
            return [int]$LASTEXITCODE
        }
    }
    return 2
}

try {
    if ([string]::IsNullOrWhiteSpace($root)) { throw 'output directory is required' }
    if (-not (Test-Path -LiteralPath $root)) { New-Item -ItemType Directory -Path $root -Force | Out-Null }
    $existing = Read-CaptureJson $statePath
    if ($null -ne $existing) {
        # schemaVersion 2 uses captureSessionToken. Keep reading the old
        # sessionToken spelling so a previous incomplete attempt cannot crash
        # the new start path under strict property checking.
        $existingToken = ''
        if ($existing.PSObject.Properties.Name -contains 'captureSessionToken') {
            $existingToken = [string]$existing.captureSessionToken
        } elseif ($existing.PSObject.Properties.Name -contains 'sessionToken') {
            $existingToken = [string]$existing.sessionToken
        }
        $sameSession = ($existingToken -eq $sessionToken -and [string]$existing.trialId -eq $TrialId)
        if (-not $sameSession) {
            $rejected = Join-Path $root ("capture-state-rejected-" + $TrialId + '.json')
            Write-CaptureAtomicJson $rejected ([ordered]@{schemaVersion=2;status='failed';reason='output_directory_belongs_to_another_session';trialId=$TrialId;captureSessionToken=$sessionToken;existingTrialId=$existing.trialId;existingCaptureSessionToken=$existingToken;createdUtc=(Get-CaptureUtcNow).ToString('o')})
            Write-Error 'output directory belongs to another capture session'
            exit 2
        }
    } elseif (@(Get-ChildItem -LiteralPath $root -Force -ErrorAction SilentlyContinue).Count -gt 0) {
        $rejected = Join-Path $root ("capture-state-rejected-" + $TrialId + '.json')
        Write-CaptureAtomicJson $rejected ([ordered]@{schemaVersion=2;status='failed';reason='output_directory_is_not_new';trialId=$TrialId;sessionToken=$sessionToken;createdUtc=(Get-CaptureUtcNow).ToString('o')})
        Write-Error 'output directory is not new and has no matching session token'
        exit 2
    }

    $state = [ordered]@{
        schemaVersion = 2
        purpose = 'system capture diagnostics'
        status = 'idle'
        phase = 'idle'
        trialId = $TrialId
        captureSessionToken = $sessionToken
        outputDirectory = $root
        channelDirectory = $ChannelDir
        applicationRequestPath = $ApplicationRequestPath
        runId = $RunId
        listenId = $ListenId
        measurementSessionId = $MeasurementSessionId
        cardIPs = @($CardIPs)
        ports = @($Ports)
        wprProfile = $WprProfile
        noWpr = [bool]$NoWpr
        simulation = $adapterSimulation
        captureMode = if ($NoWpr) { 'pktmon-only' } else { 'pktmon+wpr' }
        resources = [ordered]@{pktmonStarted=$false;wprStarted=$false;filterNames=@();ownerVerified=$false}
        commands = $commands
        lifecycle = $lifecycle
        failureReasons = $failures
        files = @()
        createdUtc = (Get-CaptureUtcNow).ToString('o')
        createdQpc = Get-CaptureQpc
    }
    Save-State 'idle'
    $lock = New-CaptureLock $root
    if ($null -eq $lock) { Fail-Start 'capture session is already being started or stopped'; exit 2 }

    Save-State 'preflight'
    if ($MaxWaitSeconds -lt 1) { Fail-Start 'max wait must be at least one second'; exit 2 }
    if (@($CardIPs).Count -eq 0 -or @($CardIPs | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -gt 0) {
        $requestForIPs = Read-ApplicationHandshake
        if ($null -ne $requestForIPs) { $CardIPs = Resolve-StringArray $requestForIPs.cardIPs }
    }
    if (@($CardIPs).Count -eq 0) { Fail-Start 'target card IPs are required; empty CardIPs never expands the capture'; exit 2 }
    $invalidIPs = @($CardIPs | Where-Object { $_ -notmatch '^\d{1,3}(\.\d{1,3}){3}$' })
    if ($invalidIPs.Count -gt 0) { Fail-Start ('invalid target IPv4 address: ' + ($invalidIPs -join ',')); exit 2 }
    $Ports = @($Ports | ForEach-Object { [int]$_ } | Select-Object -Unique)
    if ($Ports.Count -eq 0 -or @($Ports | Where-Object { $_ -lt 8000 -or $_ -gt 8004 }).Count -gt 0) {
        Fail-Start 'target UDP ports must be within 8000-8004'; exit 2
    }

    $request = Read-ApplicationHandshake
    if ($null -eq $request) { Fail-Start 'application handshake request is missing or unreadable'; exit 2 }
    if (-not [string]::IsNullOrWhiteSpace($ApplicationRequestPath)) { $resolvedApplicationRequestPath = $ApplicationRequestPath }
    elseif (-not [string]::IsNullOrWhiteSpace($script:resolvedApplicationRequestPath)) { $resolvedApplicationRequestPath = $script:resolvedApplicationRequestPath }
    if (-not [string]::IsNullOrWhiteSpace([string]$request.trialId) -and [string]$request.trialId -ne $TrialId) { Fail-Start 'application handshake trialId does not match'; exit 2 }
    $requestToken = [string]$request.captureSessionToken
    if ([string]::IsNullOrWhiteSpace($requestToken)) { Fail-Start 'application handshake captureSessionToken is missing'; exit 2 }
    if ($explicitSessionToken -and $requestToken -ne $sessionToken) { Fail-Start 'application handshake captureSessionToken does not match the requested session'; exit 2 }
    if ($requestToken -ne $sessionToken) {
        $sessionToken = $requestToken
        $state.captureSessionToken = $sessionToken
    }
    if ([string]::IsNullOrWhiteSpace($RunId)) { $RunId = [string]$request.runId }
    if ([string]::IsNullOrWhiteSpace($ListenId)) { $ListenId = [string]$request.listenId }
    if ([string]::IsNullOrWhiteSpace($MeasurementSessionId)) { $MeasurementSessionId = [string]$request.measurementSessionId }
    if ([string]::IsNullOrWhiteSpace($RunId) -or [string]::IsNullOrWhiteSpace($ListenId)) { Fail-Start 'application handshake runId/listenId is missing'; exit 2 }
    $requestIPs = @(Resolve-StringArray $request.cardIPs)
    if ($requestIPs.Count -gt 0 -and (($requestIPs -join ',') -ne (@($CardIPs) -join ','))) { Fail-Start 'application handshake card IPs do not match requested filters'; exit 2 }
    $requestPorts = @($request.ports | ForEach-Object { [int]$_ })
    if ($requestPorts.Count -gt 0 -and (($requestPorts | Sort-Object) -join ',') -ne ((@($Ports) | Sort-Object) -join ',')) { Fail-Start 'application handshake ports do not match requested filters'; exit 2 }
    $requestOutput = [string]$request.outputDirectory
    if (-not [string]::IsNullOrWhiteSpace($requestOutput) -and (Get-FullPathSafe $requestOutput) -ne $root) { Fail-Start 'application handshake output directory does not match'; exit 2 }
    $state.runId = $RunId; $state.listenId = $ListenId; $state.measurementSessionId = $MeasurementSessionId; $state.applicationRequestPath = $resolvedApplicationRequestPath
    $state.cardIPs = @($CardIPs); $state.ports = @($Ports); $state.applicationRequestPath = $resolvedApplicationRequestPath
    Save-StateWithoutTransition

    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    $isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    $state.permission = [ordered]@{user=$identity.Name;isAdministrator=$isAdmin}
    if (-not $isAdmin -and -not $adapterSimulation) { Fail-Start 'administrator privileges are required for system capture'; exit 2 }
    if (-not $isAdmin -and $adapterSimulation) { $state.permission.simulationBypass = $true }

    $help = Add-Command (Invoke-CaptureProcess 'pktmon.exe' @('help') 'validate pktmon command surface' 15 $AdapterPath)
    $filterHelp = Add-Command (Invoke-CaptureProcess 'pktmon.exe' @('filter','add','help') 'validate pktmon filter arguments' 15 $AdapterPath)
    $startHelp = Add-Command (Invoke-CaptureProcess 'pktmon.exe' @('start','help') 'validate pktmon start arguments' 15 $AdapterPath)
    if ($help.timedOut -or $filterHelp.timedOut -or $startHelp.timedOut -or $help.exitCode -ne 0 -or $filterHelp.exitCode -ne 0 -or $startHelp.exitCode -ne 0) { Fail-Start 'pktmon help command failed'; exit 2 }
    if (-not ((Get-CaptureCommandText $startHelp) -match '(?i)pkt-size') -or -not ((Get-CaptureCommandText $startHelp) -match '(?i)log-mode')) { Fail-Start 'pktmon help did not confirm required capture arguments'; exit 2 }
    if (-not $NoWpr) {
        $wprHelp = Add-Command (Invoke-CaptureProcess 'wpr.exe' @('-help') 'validate WPR command surface' 15 $AdapterPath)
        if ($wprHelp.timedOut -or $wprHelp.exitCode -ne 0) { Fail-Start 'WPR help command failed'; exit 2 }
    }

    $wprStateRecord = $null
    if (-not $NoWpr) {
        $wprStateRecord = Add-Command (Invoke-CaptureProcess 'wpr.exe' @('-status') 'check existing WPR session' 15 $AdapterPath)
        $wprState = Get-ToolState $wprStateRecord 'wpr'
        $state.preflightWprStatus = $wprState
        if ($wprState -eq 'command_failed') { Fail-Start 'WPR status command_failed'; exit 2 }
        if ($wprState -eq 'unknown') { Fail-Start 'WPR status unknown; refusing to classify or stop an external session'; exit 2 }
        if ($wprState -eq 'running') { Fail-Start 'WPR status busy; an existing session is recording'; exit 2 }
    } else {
        $state.wpr = [ordered]@{status='skipped_by_operator';reason='NoWpr'}
    }
    $pktmonStatusRecord = Add-Command (Invoke-CaptureProcess 'pktmon.exe' @('status') 'check existing pktmon session' 15 $AdapterPath)
    $pktmonState = Get-ToolState $pktmonStatusRecord 'pktmon'
    $state.preflightPktmonStatus = $pktmonState
    if ($pktmonState -eq 'command_failed') { Fail-Start 'Pktmon status command_failed'; exit 2 }
    if ($pktmonState -eq 'unknown') { Fail-Start 'Pktmon status unknown; refusing to classify or stop an external session'; exit 2 }
    if ($pktmonState -eq 'running') { Fail-Start 'Pktmon status busy; an existing session is recording'; exit 2 }
    $filterListRecord = Add-Command (Invoke-CaptureProcess 'pktmon.exe' @('filter','list') 'check existing pktmon filters' 15 $AdapterPath)
    $filterInventory = Get-FilterInventory $filterListRecord
    $state.preflightFilterStatus = $filterInventory.status
    if (-not (Test-FilterInventoryEmpty $filterInventory)) { Fail-Start ('pktmon filter preflight is ' + $filterInventory.status + '; refusing to remove foreign filters'); exit 2 }
    Save-StateWithoutTransition

    Save-State 'starting'
    $expectedFilters = New-CaptureFilterSpecs $CardIPs $Ports
    foreach ($filter in @($expectedFilters)) {
        $addRecord = Add-Command (Invoke-CaptureProcess 'pktmon.exe' ([string[]]$filter.arguments) ("add filter " + $filter.name) 15 $AdapterPath)
        if ($addRecord.timedOut -or $addRecord.exitCode -ne 0) {
            Fail-Start ('filter add failed: ' + $filter.name)
            $state.resources.filterNames = @($filterNames); $state.filterNames = @($filterNames); Save-StateWithoutTransition
            $stopCode = Stop-AfterStartFailure; exit $stopCode
        }
        $filterNames.Add([string]$filter.name)
        $state.resources.filterNames = @($filterNames); $state.filterNames = @($filterNames)
        Save-StateWithoutTransition
    }
    $afterAddRecord = Add-Command (Invoke-CaptureProcess 'pktmon.exe' @('filter','list') 'verify actual pktmon filter set' 15 $AdapterPath)
    $afterAddInventory = Get-FilterInventory $afterAddRecord
    $state.filterInventoryAfterAdd = $afterAddInventory
    if (-not (Test-FilterInventoryExact $afterAddInventory $expectedFilters)) {
        Fail-Start 'actual pktmon filter set did not match requested card IP/UDP port filters'
        $state.resources.filterNames = @($filterNames); Save-StateWithoutTransition
        $stopCode = Stop-AfterStartFailure; exit $stopCode
    }

    $etl = Join-Path $root 'pktmon.etl'
    $pktmonArgs = @('start','--capture','--comp','all','--type','all','--pkt-size','128','--log-mode','memory','--file-size','256','--file-name',$etl)
    $pktmonStart = Add-Command (Invoke-CaptureProcess 'pktmon.exe' $pktmonArgs 'start pktmon all-components flow-drop memory capture' 30 $AdapterPath)
    if ($pktmonStart.timedOut -or $pktmonStart.exitCode -ne 0) {
        Fail-Start 'pktmon start failed'
        $stopCode = Stop-AfterStartFailure; exit $stopCode
    }
    $pktmonStarted = $true; $state.resources.pktmonStarted = $true; $state.resources.ownerVerified = $true; Save-StateWithoutTransition
    $postStartStatus = Add-Command (Invoke-CaptureProcess 'pktmon.exe' @('status') 'confirm pktmon started' 15 $AdapterPath)
    $postPktmonState = Get-ToolState $postStartStatus 'pktmon'
    if ($postPktmonState -ne 'running') { Fail-Start ('pktmon post-start status is ' + $postPktmonState); $stopCode = Stop-AfterStartFailure; exit $stopCode }
    $components = Add-Command (Invoke-CaptureProcess 'pktmon.exe' @('list','--json') 'record observable pktmon components' 15 $AdapterPath)
    $state.components = if ($components.exitCode -eq 0 -and -not $components.timedOut) { $components.stdout } else { 'unknown' }
    Save-StateWithoutTransition

    if (-not $NoWpr) {
        $wprStart = Add-Command (Invoke-CaptureProcess 'wpr.exe' @('-start',$WprProfile) ('start WPR memory profile ' + $WprProfile) 30 $AdapterPath)
        if ($wprStart.timedOut -or $wprStart.exitCode -ne 0) { Fail-Start 'WPR start failed; automatic NoWpr downgrade is not allowed'; $stopCode = Stop-AfterStartFailure; exit $stopCode }
        $wprStarted = $true; $state.resources.wprStarted = $true; Save-StateWithoutTransition
        $postWprStatus = Add-Command (Invoke-CaptureProcess 'wpr.exe' @('-status') 'confirm WPR started' 15 $AdapterPath)
        $postWprState = Get-ToolState $postWprStatus 'wpr'
        if ($postWprState -ne 'running') { Fail-Start ('WPR post-start status is ' + $postWprState); $stopCode = Stop-AfterStartFailure; exit $stopCode }
    }

    $readyUtc = Get-CaptureUtcNow
    $readyQpc = Get-CaptureQpc
    $readyNs = Get-CaptureMonotonicNs
    $state.ready = [ordered]@{utc=$readyUtc.ToString('o');qpc=$readyQpc;monotonicNs=$readyNs;pktmonConfirmed=$true;wprConfirmed=([bool]$NoWpr -or $wprStarted);applicationHandshakeMatched=$true}
    $state.resources.ownerVerified = $true
    Write-CaptureAtomicJson (Join-Path $root 'capture-ready.json') ([ordered]@{schemaVersion=1;trialId=$TrialId;captureSessionToken=$sessionToken;runId=$RunId;listenId=$ListenId;measurementSessionId=$MeasurementSessionId;readyUtc=$readyUtc.ToString('o');readyQpc=$readyQpc;readyMonotonicNs=$readyNs;captureMode=$state.captureMode;wpr=if($NoWpr){'skipped_by_operator'}else{'started'}})
    Save-State 'ready'
    Save-State 'collecting'

    $activeTrialPath = Join-Path $ChannelDir 'active-trial.json'
    if (-not (Test-Path -LiteralPath $ChannelDir)) { New-Item -ItemType Directory -Path $ChannelDir -Force | Out-Null }
    $startedWallMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $active = [ordered]@{schemaVersion=2;trialId=$TrialId;captureSessionToken=$sessionToken;runId=$RunId;listenId=$ListenId;measurementSessionId=$MeasurementSessionId;startedWallMs=$startedWallMs;expiresWallMs=($startedWallMs + ($MaxWaitSeconds + 30) * 1000);outputDirectory=$root;channelDirectory=$ChannelDir}
    Write-CaptureAtomicJson $activeTrialPath $active
    $state.activeTrialPath = $activeTrialPath; Save-StateWithoutTransition

    $notification = $null
    $deadline = (Get-CaptureUtcNow).AddSeconds($MaxWaitSeconds)
    $stopDeadlineNs = $null
    while ((Get-CaptureUtcNow) -lt $deadline) {
        $files = @(Get-ChildItem -LiteralPath $ChannelDir -Filter 'burst-*.json' -File -ErrorAction SilentlyContinue)
        foreach ($candidate in $files) {
            $note = Read-CaptureJson $candidate.FullName
            if ($null -eq $note) { continue }
            $burstNs = 0L
            try { $burstNs = [Int64]$note.burstMonotonicNs } catch { $burstNs = 0L }
            $matches = ([string]$note.kind -eq 'burst' -and [string]$note.trialId -eq $TrialId -and
                [string]$note.runId -eq $RunId -and [string]$note.captureSessionToken -eq $sessionToken -and
                $burstNs -ge [Int64]$state.ready.monotonicNs)
            if ($matches) { $notification = $note; break }
        }
        if ($null -ne $notification) {
            $burstNs = [Int64]$notification.burstMonotonicNs
            $stopDeadlineNs = $burstNs + 10000000000L
            $state.notification = $notification; $state.stopDeadlineMonotonicNs = $stopDeadlineNs; Save-StateWithoutTransition
            while ((Get-CaptureMonotonicNs) -lt $stopDeadlineNs) { Start-Sleep -Milliseconds 100 }
            break
        }
        Start-Sleep -Milliseconds 200
    }
    if ($null -eq $notification) { $state.stoppedBy = 'timeout'; $state.notification = $null } else { $state.stoppedBy = 'application-burst-after-10s' }
    Save-State 'stopping'
    Release-CaptureLock $lock
    $lock = $null
    & $stopScript -StatePath $statePath -AdapterPath $AdapterPath | Out-Host
    $stopCode = [int]$LASTEXITCODE
    exit $stopCode
} catch {
    $message = 'start script exception at line ' + $_.InvocationInfo.ScriptLineNumber + ': ' + $_.Exception.Message
    try {
        if ($null -ne $state) { Add-CaptureFailure $state.failureReasons $message; $state.commands = $commands; $state.resources.pktmonStarted = $pktmonStarted; $state.resources.wprStarted = $wprStarted; Save-State 'failed' }
    } catch {}
    if ($pktmonStarted -or $wprStarted -or $filterNames.Count -gt 0) {
        try {
            Release-CaptureLock $lock
            $lock = $null
            & $stopScript -StatePath $statePath -AdapterPath $AdapterPath -StartFailure | Out-Host
        } catch {}
    }
    Write-Error $message
    exit 2
} finally {
    Release-CaptureLock $lock
}
