param(
    [Parameter(Mandatory=$true)][string]$Program,
    [Parameter(Mandatory=$true)][string]$ArgumentsJson,
    [string]$Description = ''
)

$ErrorActionPreference = 'Stop'
$testRoot = [Environment]::GetEnvironmentVariable('SYSTEM_CAPTURE_TEST_ROOT')
if ([string]::IsNullOrWhiteSpace($testRoot)) { throw 'SYSTEM_CAPTURE_TEST_ROOT is required by the synthetic adapter' }
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$adapterStatePath = Join-Path $testRoot 'adapter-state.json'
$state = $null
if (Test-Path -LiteralPath $adapterStatePath) {
    $state = Get-Content -Raw -LiteralPath $adapterStatePath | ConvertFrom-Json
}
if ($null -eq $state) {
    $state = [pscustomobject]@{pktmonRunning=$false;wprRunning=$false;filters=@();components=@('all')}
}
$arguments = @($ArgumentsJson | ConvertFrom-Json | ForEach-Object { [string]$_ })
$lowerProgram = [IO.Path]::GetFileName($Program).ToLowerInvariant()

function Save-AdapterState {
    $state | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $adapterStatePath -Encoding utf8
}

function Write-U16([System.Collections.Generic.List[byte]]$Buffer, [uint16]$Value) {
    [void]$Buffer.Add([byte]($Value -band 0xff))
    [void]$Buffer.Add([byte](($Value -shr 8) -band 0xff))
}
function Write-U32([System.Collections.Generic.List[byte]]$Buffer, [uint32]$Value) {
    [void]$Buffer.Add([byte]($Value -band 0xff))
    [void]$Buffer.Add([byte](($Value -shr 8) -band 0xff))
    [void]$Buffer.Add([byte](($Value -shr 16) -band 0xff))
    [void]$Buffer.Add([byte](($Value -shr 24) -band 0xff))
}
function Add-Block([System.Collections.Generic.List[byte]]$Buffer, [uint32]$Type, [System.Collections.Generic.List[byte]]$Body) {
    while (($Body.Count % 4) -ne 0) { [void]$Body.Add([byte]0) }
    $length = [uint32](12 + $Body.Count)
    Write-U32 $Buffer $Type
    Write-U32 $Buffer $length
    foreach ($value in $Body) { [void]$Buffer.Add([byte]$value) }
    Write-U32 $Buffer $length
}
function New-SyntheticPcapNg([string]$Path) {
    $buffer = [System.Collections.Generic.List[byte]]::new()
    $shb = [System.Collections.Generic.List[byte]]::new()
    Write-U32 $shb 0x1A2B3C4D
    Write-U16 $shb 1; Write-U16 $shb 0
    Write-U32 $shb ([uint32]::MaxValue); Write-U32 $shb ([uint32]::MaxValue)
    Add-Block $buffer 0x0A0D0D0A $shb
    $idb = [System.Collections.Generic.List[byte]]::new()
    Write-U16 $idb 1; Write-U16 $idb 0; Write-U32 $idb 65535
    Add-Block $buffer 1 $idb
    foreach ($timestamp in @(1000000,1005000)) {
        $packet = [System.Collections.Generic.List[byte]]::new()
        foreach ($value in @(0,1,2,3,4,5,6,7,8,9,10,11,0x08,0x00)) { [void]$packet.Add([byte]$value) }
        foreach ($value in @(0x45,0,0,32,0,0,0,0,64,17,0,0,192,168,0,1,192,168,0,2)) { [void]$packet.Add([byte]$value) }
        foreach ($value in @(0x9c,0x40,0x1f,0x40,0,12,0,0,1,2,3,4)) { [void]$packet.Add([byte]$value) }
        $epb = [System.Collections.Generic.List[byte]]::new()
        Write-U32 $epb 0
        Write-U32 $epb ([uint32]([uint64]$timestamp -shr 32)); Write-U32 $epb ([uint32]$timestamp)
        Write-U32 $epb ([uint32]$packet.Count); Write-U32 $epb ([uint32]$packet.Count)
        foreach ($value in $packet) { [void]$epb.Add([byte]$value) }
        Add-Block $buffer 6 $epb
    }
    [IO.File]::WriteAllBytes($Path, $buffer.ToArray())
}
function Get-ArgumentValue([string]$Name, [string]$Default = '') {
    $index = [Array]::IndexOf($arguments, $Name)
    if ($index -ge 0 -and $index + 1 -lt $arguments.Count) { return [string]$arguments[$index + 1] }
    return $Default
}
function Emit-Json($Value) { $Value | ConvertTo-Json -Compress -Depth 20 | Write-Output }

if ($lowerProgram -eq 'pktmon.exe') {
    if ($arguments.Count -eq 0 -or $arguments[0] -eq 'help') {
        Write-Output 'pktmon help: capture filter start stop status'
    } elseif ($arguments[0] -eq 'filter' -and $arguments.Count -ge 2 -and $arguments[1] -eq 'add' -and $arguments.Count -eq 3) {
        Write-Output 'filter add help: -t UDP -p port -i address'
    } elseif ($arguments[0] -eq 'filter' -and $arguments[1] -eq 'add') {
        $name = $arguments[2]
        $ip = Get-ArgumentValue '-i'
        $port = Get-ArgumentValue '-p'
        $filters = @($state.filters | Where-Object { [string]$_.name -ne $name })
        $filters += [pscustomobject]@{name=$name;protocol='UDP';port=$port;ip=$ip}
        $state.filters = @($filters)
        Save-AdapterState
        Write-Output 'Filter added.'
    } elseif ($arguments[0] -eq 'filter' -and $arguments[1] -eq 'list') {
        Emit-Json ([ordered]@{filters=@($state.filters)})
    } elseif ($arguments[0] -eq 'filter' -and $arguments[1] -eq 'remove' -and $arguments.Count -eq 3 -and $arguments[2] -eq 'help') {
        Write-Output 'filter remove help: filter remove name'
    } elseif ($arguments[0] -eq 'filter' -and $arguments[1] -eq 'remove' -and $arguments.Count -ge 3) {
        $name = $arguments[2]
        $state.filters = @($state.filters | Where-Object { [string]$_.name -ne $name })
        Save-AdapterState
        Write-Output 'Filter removed.'
    } elseif ($arguments[0] -eq 'filter' -and $arguments[1] -eq 'remove') {
        $state.filters = @()
        Save-AdapterState
        Write-Output 'Filters removed.'
    } elseif ($arguments[0] -eq 'status') {
        if ([bool]$state.pktmonRunning) { Write-Output 'Pktmon is running.' } else { Write-Output 'Pktmon is stopped.' }
    } elseif ($arguments[0] -eq 'start' -and $arguments[1] -eq 'help') {
        Write-Output 'start help: --capture --comp --type --pkt-size --log-mode --file-size --file-name'
    } elseif ($arguments[0] -eq 'start') {
        $etlPath = Get-ArgumentValue '--file-name'
        if (-not [string]::IsNullOrWhiteSpace($etlPath)) { [IO.File]::WriteAllBytes($etlPath, [Text.Encoding]::ASCII.GetBytes('synthetic pktmon etl')) }
        $state.pktmonRunning = $true
        Save-AdapterState
        Write-Output 'Pktmon is recording.'
    } elseif ($arguments[0] -eq 'stop') {
        $state.pktmonRunning = $false
        Save-AdapterState
        Write-Output 'Pktmon stopped.'
    } elseif ($arguments[0] -eq 'list') {
        Emit-Json ([ordered]@{components=@('NIC','IP','UDP','drop')})
    } elseif ($arguments[0] -eq 'etl2txt') {
        $output = Get-ArgumentValue '-o'
        if ([string]::IsNullOrWhiteSpace($output)) { $output = Get-ArgumentValue '--out' }
        if ($arguments -contains '--drop-only') { Set-Content -LiteralPath $output -Value 'Dropped: 0' -Encoding utf8 } else { Set-Content -LiteralPath $output -Value 'Packets: 2`nDropped: 0' -Encoding utf8 }
        Write-Output 'ETL converted to text.'
    } elseif ($arguments[0] -eq 'etl2pcap') {
        $output = Get-ArgumentValue '-o'
        if ([string]::IsNullOrWhiteSpace($output)) { $output = Get-ArgumentValue '--out' }
        New-SyntheticPcapNg $output
        Write-Output 'ETL converted to PcapNG.'
    } else {
        Write-Output 'pktmon command accepted'
    }
} elseif ($lowerProgram -eq 'wpr.exe') {
    if ($arguments -contains '-help' -or $arguments -contains '/?') {
        Write-Output 'WPR help: -start -stop -status'
    } elseif ($arguments -contains '-profiledetails') {
        Write-Output 'GeneralProfile details'
    } elseif ($arguments -contains '-status') {
        if ([bool]$state.wprRunning) { Write-Output 'WPR is recording.' } else { Write-Output 'WPR is not recording.' }
    } elseif ($arguments -contains '-start') {
        $state.wprRunning = $true
        Save-AdapterState
        Write-Output 'WPR is recording.'
    } elseif ($arguments -contains '-stop') {
        $output = $arguments[$arguments.IndexOf('-stop') + 1]
        [IO.File]::WriteAllBytes($output, [Text.Encoding]::ASCII.GetBytes('synthetic wpr etl'))
        $state.wprRunning = $false
        Save-AdapterState
        Write-Output 'WPR stopped.'
    } else {
        Write-Output 'wpr command accepted'
    }
} else {
    Write-Output 'command accepted'
}
