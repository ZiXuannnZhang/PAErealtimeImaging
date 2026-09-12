Set-StrictMode -Version Latest

function Get-CaptureUtcNow {
    return [DateTime]::UtcNow
}

function Get-CaptureQpc {
    return [Diagnostics.Stopwatch]::GetTimestamp()
}

function Get-CaptureQpcFrequency {
    return [Diagnostics.Stopwatch]::Frequency
}

function Get-CaptureMonotonicNs {
    $ticks = [Diagnostics.Stopwatch]::GetTimestamp()
    return [Int64](($ticks * 1000000000L) / [Diagnostics.Stopwatch]::Frequency)
}

function New-CaptureId {
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
    return "$stamp-$([Guid]::NewGuid().ToString('N'))"
}

function Get-FullPathSafe([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return '' }
    return [IO.Path]::GetFullPath($Path)
}

function Write-CaptureAtomicText([string]$Path, [string]$Text) {
    $full = Get-FullPathSafe $Path
    $directory = Split-Path -Parent $full
    if (-not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    $leaf = Split-Path -Leaf $full
    $temp = Join-Path $directory ('.' + $leaf + '.' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($temp, $Text, $utf8)
    try {
        if (Test-Path -LiteralPath $full) {
            try {
                [IO.File]::Replace($temp, $full, $null, $true)
            } catch {
                Move-Item -LiteralPath $temp -Destination $full -Force
            }
        } else {
            [IO.File]::Move($temp, $full)
        }
    } finally {
        if (Test-Path -LiteralPath $temp) {
            Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue
        }
    }
}

function Write-CaptureAtomicJson([string]$Path, $Value, [int]$Depth = 32) {
    $json = $Value | ConvertTo-Json -Depth $Depth
    Write-CaptureAtomicText $Path $json
}

function Read-CaptureJson([string]$Path, [int]$Retries = 8) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    for ($attempt = 0; $attempt -lt $Retries; ++$attempt) {
        try {
            $bytes = [IO.File]::ReadAllBytes($Path)
            $text = [Text.Encoding]::UTF8.GetString($bytes)
            if ([string]::IsNullOrWhiteSpace($text)) { throw 'empty JSON file' }
            return $text | ConvertFrom-Json
        } catch {
            if ($attempt + 1 -ge $Retries) { return $null }
            Start-Sleep -Milliseconds ([Math]::Min(250, 10 * ($attempt + 1)))
        }
    }
    return $null
}

function ConvertTo-ArgumentString([string[]]$Arguments) {
    $parts = [System.Collections.Generic.List[string]]::new()
    foreach ($argument in @($Arguments)) {
        $value = [string]$argument
        if ($value -notmatch '[\s"]') {
            $parts.Add($value)
            continue
        }
        $escaped = $value.Replace('\', '\\').Replace('"', '\"')
        $parts.Add('"' + $escaped + '"')
    }
    return ($parts -join ' ')
}

function Invoke-CaptureProcess {
    param(
        [Parameter(Mandatory=$true)][string]$Program,
        [Parameter(Mandatory=$true)][string[]]$Arguments,
        [Parameter(Mandatory=$true)][string]$Description,
        [int]$TimeoutSeconds = 15,
        [string]$AdapterPath = '',
        [string]$WorkingDirectory = ''
    )
    $startUtc = Get-CaptureUtcNow
    $startQpc = Get-CaptureQpc
    $invokeProgram = $Program
    [string[]]$invokeArguments = @($Arguments)
    $simulation = $false
    if ([string]::IsNullOrWhiteSpace($AdapterPath)) {
        $configured = [Environment]::GetEnvironmentVariable('SYSTEM_CAPTURE_COMMAND_ADAPTER')
        if (-not [string]::IsNullOrWhiteSpace($configured)) { $AdapterPath = $configured }
    }
    if (-not [string]::IsNullOrWhiteSpace($AdapterPath)) {
        $simulation = $true
        $invokeProgram = (Get-Command pwsh.exe -ErrorAction SilentlyContinue).Source
        if ([string]::IsNullOrWhiteSpace($invokeProgram)) { $invokeProgram = (Get-Command powershell.exe -ErrorAction SilentlyContinue).Source }
        $argumentsJson = (@($Arguments) | ConvertTo-Json -Compress)
        $invokeArguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$AdapterPath,
            '-Program',$Program,'-ArgumentsJson',$argumentsJson,'-Description',$Description)
    }
    $record = [ordered]@{
        id = [Guid]::NewGuid().ToString('N')
        description = $Description
        program = $Program
        arguments = @($Arguments)
        invocationProgram = $invokeProgram
        invocationArguments = @($invokeArguments)
        timeoutSeconds = $TimeoutSeconds
        startUtc = $startUtc.ToString('o')
        startQpc = $startQpc
        exitCode = $null
        stdout = ''
        stderr = ''
        timedOut = $false
        simulation = $simulation
    }
    try {
        if ([string]::IsNullOrWhiteSpace($invokeProgram) -or -not (Test-Path -LiteralPath $invokeProgram)) {
            $found = Get-Command $invokeProgram -ErrorAction SilentlyContinue
            if ($found) { $invokeProgram = $found.Source; $record.invocationProgram = $invokeProgram }
        }
        $psi = [Diagnostics.ProcessStartInfo]::new()
        $psi.FileName = $invokeProgram
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) { $psi.WorkingDirectory = $WorkingDirectory }
        if ($psi.PSObject.Properties.Name -contains 'ArgumentList') {
            foreach ($argument in @($invokeArguments)) { [void]$psi.ArgumentList.Add([string]$argument) }
        } else {
            $psi.Arguments = ConvertTo-ArgumentString @($invokeArguments)
        }
        # Current Windows pktmon/wpr builds emit localized text as UTF-8 when
        # stdout is redirected. An unset ProcessStartInfo encoding can fall
        # back to the local ANSI/OEM page, turning Chinese state text into
        # mojibake and making a stopped tool look like an unknown state.
        if ($psi.PSObject.Properties.Name -contains 'StandardOutputEncoding') {
            try {
                $toolEncoding = [Text.Encoding]::UTF8
                $psi.StandardOutputEncoding = $toolEncoding
                if ($psi.PSObject.Properties.Name -contains 'StandardErrorEncoding') {
                    $psi.StandardErrorEncoding = $toolEncoding
                }
            } catch {
                # Keep the runtime default encoding if UTF-8 is unavailable.
            }
        }
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $psi
        if (-not $process.Start()) { throw "failed to start $invokeProgram" }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $record.timedOut = $true
            try { $process.Kill($true) } catch { try { $process.Kill() } catch {} }
            $process.WaitForExit(2000) | Out-Null
            $record.exitCode = -1
        } else {
            $record.exitCode = $process.ExitCode
        }
        $record.stdout = $stdoutTask.Result
        $record.stderr = $stderrTask.Result
        $process.Dispose()
    } catch {
        $record.exitCode = -1
        $record.stderr = $_.Exception.Message
    }
    $endUtc = Get-CaptureUtcNow
    $record.endUtc = $endUtc.ToString('o')
    $record.endQpc = Get-CaptureQpc
    $record.durationMs = [Math]::Round(($endUtc - $startUtc).TotalMilliseconds, 3)
    $record.command = $Program + ' ' + (ConvertTo-ArgumentString @($Arguments))
    return $record
}

function Get-CaptureCommandText($Record) {
    if ($null -eq $Record) { return '' }
    return ([string]$Record.stdout + "`n" + [string]$Record.stderr)
}

function Get-CaptureProperty($Value, [string]$Name, $Default = $null) {
    if ($null -eq $Value) { return $Default }
    if ($Value -is [System.Collections.IDictionary]) {
        if ($Value.Contains($Name)) { return $Value[$Name] }
    } elseif ($Value.PSObject.Properties.Name -contains $Name) { return $Value.$Name }
    return $Default
}

function Test-CaptureRequestLive($Request) {
    # File mtime does not identify a running application. Also check creation
    # time against process birth so a recycled PID cannot revive an old trial.
    $result = [ordered]@{valid=$false;reason='application request is missing or invalid'}
    if ($null -eq $Request) { return $result }
    foreach ($name in @('trialId','captureSessionToken','runId','listenId','outputDirectory','channelDirectory')) {
        if ([string]::IsNullOrWhiteSpace([string](Get-CaptureProperty $Request $name ''))) {
            $result.reason = "application request is missing $name"; return $result
        }
    }
    if ((Get-CaptureProperty $Request 'active' $true) -ne $true) {
        $result.reason = 'application request is inactive; prepare system capture in the current listener'; return $result
    }
    $applicationPid = 0
    $created = [DateTimeOffset]::MinValue
    if (-not [int]::TryParse([string](Get-CaptureProperty $Request 'applicationPid' ''), [ref]$applicationPid) -or $applicationPid -le 0 -or
        -not [DateTimeOffset]::TryParse([string](Get-CaptureProperty $Request 'createdUtc' ''), [ref]$created)) {
        $result.reason = 'application process identity is missing; prepare a new request in the current application'; return $result
    }
    try {
        $applicationProcess = Get-Process -Id $applicationPid -ErrorAction Stop
        $birth = $applicationProcess.StartTime.ToUniversalTime()
        if ($applicationProcess.HasExited -or $created.UtcDateTime -lt $birth -or $created -gt [DateTimeOffset]::UtcNow.AddSeconds(5)) {
            $result.reason = "stale request: application PID $applicationPid was restarted"; return $result
        }
        $expectedExecutable = [string](Get-CaptureProperty $Request 'applicationExecutable' '')
        if ($expectedExecutable -and (Get-FullPathSafe $applicationProcess.Path) -ne (Get-FullPathSafe $expectedExecutable)) {
            $result.reason = 'application executable does not match the request'; return $result
        }
    } catch {
        $result.reason = "stale or unverifiable request: application PID $applicationPid is not available; prepare system capture in the current application"
        return $result
    }
    $result.valid = $true; $result.reason = ''; return $result
}

function Resolve-CaptureApplicationRequest {
    param([string]$ChannelDir, [string]$RequestPath = '', [string]$OutputDirectory = '', [string]$TrialId = '')
    $candidates = @()
    if ($RequestPath) { $candidates = @(Get-FullPathSafe $RequestPath) }
    elseif (Test-Path -LiteralPath $ChannelDir) {
        $candidates = @(Get-ChildItem -LiteralPath $ChannelDir -Filter 'capture-request*.json' -File | ForEach-Object { $_.FullName })
    }
    $valid = [System.Collections.Generic.List[object]]::new()
    $rejected = [System.Collections.Generic.List[string]]::new()
    foreach ($path in $candidates) {
        $value = Read-CaptureJson $path
        if ($null -eq $value) { $rejected.Add("unreadable request: $path"); continue }
        if ($TrialId -and [string](Get-CaptureProperty $value 'trialId' '') -ne $TrialId) { continue }
        if ($OutputDirectory -and (Get-FullPathSafe ([string](Get-CaptureProperty $value 'outputDirectory' ''))) -ne (Get-FullPathSafe $OutputDirectory)) { continue }
        $check = Test-CaptureRequestLive $value
        if (-not $check.valid) { $rejected.Add($check.reason); continue }
        $valid.Add([pscustomobject]@{path=$path;request=$value})
    }
    if ($valid.Count -eq 1) { return $valid[0] }
    if ($valid.Count -gt 1) { throw 'multiple live capture requests; use the command shown by the intended application (ApplicationRequestPath)' }
    $detail = (@($rejected | Select-Object -Unique) -join '; ')
    throw ("No live capture request. Start listening and click Prepare system capture in the current application, then run Open-AdminCapture.cmd. " + $detail)
}

function Get-ToolState {
    param([Parameter(Mandatory=$true)]$Record, [Parameter(Mandatory=$true)][ValidateSet('pktmon','wpr')][string]$Tool)
    if ([bool]$Record.timedOut -or [int]$Record.exitCode -ne 0) { return 'command_failed' }
    $text = Get-CaptureCommandText $Record
    if ($Tool -eq 'pktmon') {
        if ($text -match '(?im)\b(stopped|not\s+running|not\s+started)\b|已停止|未运行|未启动|没有运行') { return 'stopped' }
        if ($text -match '(?im)\b(running|recording|capturing|active)\b|正在运行|正在捕获|运行中') { return 'running' }
        # Current Windows status uses a labeled table, not the word "running".
        # Require both collection and logger fields; do not match a help page.
        if (@($Record.arguments).Count -eq 1 -and $Record.arguments[0] -eq 'status' -and
            $text -match '(?im)^\s*(Collected data|收集的数据)\s*[:：]' -and
            $text -match '(?im)^\s*(Logger name|记录程序名称)\s*[:：]\s*PktMon\s*$') { return 'running' }
    } else {
        if ($text -match '(?im)WPR\s+is\s+not\s+recording|\b(not\s+recording|stopped)\b|未记录|已停止|未运行') { return 'stopped' }
        if ($text -match '(?im)\b(recording|running|active)\b|正在记录|正在运行|采集中') { return 'running' }
    }
    return 'unknown'
}

function Get-FilterInventory {
    param([Parameter(Mandatory=$true)]$Record)
    $result = [ordered]@{ status = 'unknown'; names = @(); entries = @(); raw = (Get-CaptureCommandText $Record) }
    if ([bool]$Record.timedOut -or [int]$Record.exitCode -ne 0) { $result.status = 'command_failed'; return $result }
    $text = [string]$result.raw
    if ($text.TrimStart().StartsWith('{') -or $text.TrimStart().StartsWith('[')) {
        try { $json = ConvertFrom-Json -InputObject $text -NoEnumerate -ErrorAction Stop } catch { return $result }
        $items = $null
        if ($json -is [array]) { $items = $json }
        elseif ($null -ne $json -and $json.PSObject.Properties.Name -contains 'filters' -and $json.filters -is [array]) { $items = $json.filters }
        if ($null -eq $items) { return $result }
        foreach ($item in $items) {
            $name = [string](Get-CaptureProperty $item 'name' (Get-CaptureProperty $item 'filterName' ''))
            if ([string]::IsNullOrWhiteSpace($name)) { return $result }
            $result.names += $name
            $result.entries += [ordered]@{name=$name;protocol=[string](Get-CaptureProperty $item 'protocol' 'unknown');port=[string](Get-CaptureProperty $item 'port' 'unknown');ip=[string](Get-CaptureProperty $item 'ip' 'unknown')}
        }
        if ($items.Count -eq 0) { $result.status = 'empty' } else { $result.status = 'ok' }
        return $result
    }
    # Match the entire output, including the localized title + indented empty
    # row observed in the field. A foreign row after "None" must not be ignored.
    if ($text -match '(?is)^\s*(?:(?:Packet\s+filters?|数据包筛选器)\s*[:：]\s*(?:None|无)|no\s+filters?\.?|0\s+filters?|no\s+pktmon\s+filters?|无筛选器|没有筛选器|筛选器数量\s*[:：]\s*0)\s*$') {
        $result.status = 'empty'; return $result
    }
    $lines = @($text -split '\r?\n' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    if ($lines.Count -gt 0 -and $lines[0] -match '^(?i:Packet\s+filters?|数据包筛选器)\s*[:：]$') { $lines = @($lines | Select-Object -Skip 1) }
    if ($lines.Count -lt 2) { return $result }
    $header = $lines[0] -replace '(?i)IP\s+Address(?:es)?|IP\s*地址', 'ip'
    $header = $header -replace '(?i)\bName\b|名称', 'name' -replace '(?i)\bProtocol\b|协议', 'protocol' -replace '(?i)\bPorts?\b|端口', 'port'
    $columns = @($header -split '\s+')
    # Unknown extra filter fields cannot establish exclusive ownership.
    if (($columns | Sort-Object) -join ',' -ne '#,ip,name,port,protocol') { return $result }
    foreach ($line in @($lines | Select-Object -Skip 1)) {
        if ($line -match '^[-\s]+$') { continue }
        $cells = @($line -split '\s+')
        if ($cells.Count -ne $columns.Count) { return $result }
        $row = @{}
        for ($i=0; $i -lt $columns.Count; ++$i) { $row[$columns[$i]] = $cells[$i] }
        if ($row['#'] -notmatch '^\d+$') { return $result }
        $result.names += $row.name
        $result.entries += [ordered]@{name=$row.name;protocol=$row.protocol;ip=$row.ip;port=$row.port}
    }
    if ($result.entries.Count -gt 0) { $result.status = 'ok' }
    return $result
}

function Test-FilterInventoryEmpty($Inventory) {
    return $null -ne $Inventory -and $Inventory.status -eq 'empty'
}

function Test-FilterInventoryExact($Inventory, [object[]]$Expected) {
    if ($null -eq $Inventory -or $Inventory.status -ne 'ok') { return $false }
    $expectedNames = @($Expected | ForEach-Object { [string]$_.name })
    $actualNames = @($Inventory.names | ForEach-Object { [string]$_ })
    if ($actualNames.Count -ne @($Expected).Count -or @($Inventory.entries).Count -ne @($Expected).Count -or
        @($actualNames | Select-Object -Unique).Count -ne $actualNames.Count) { return $false }
    $actualKey = (($actualNames | Sort-Object) -join '|')
    $expectedKey = (($expectedNames | Sort-Object) -join '|')
    if ($actualKey -ne $expectedKey) { return $false }
    foreach ($item in @($Expected)) {
        $match = @($Inventory.entries | Where-Object {
            [string]$_.name -eq [string]$item.name -and
            [string]$_.port -eq [string]$item.port -and
            [string]$_.ip -eq [string]$item.ip -and
            [string]$_.protocol -match '(?i)^UDP$'
        })
        if ($match.Count -ne 1) { return $false }
    }
    return $true
}

function New-CaptureFilterSpecs {
    param([string[]]$CardIPs, [int[]]$Ports, [string]$Prefix = 'StartupDiag')
    $items = [System.Collections.Generic.List[object]]::new()
    for ($i=0; $i -lt @($CardIPs).Count; ++$i) {
        foreach ($port in @($Ports)) {
            $name = "$Prefix-$i-$port"
            $items.Add([ordered]@{name=$name;ip=[string]$CardIPs[$i];port=[int]$port;protocol='UDP';arguments=@('filter','add',$name,'-t','UDP','-p',[string]$port,'-i',[string]$CardIPs[$i])})
        }
    }
    return @($items)
}

function Get-CaptureFileRecord([string]$Root, [string]$Path, [string]$Kind) {
    $full = Get-FullPathSafe $Path
    $relative = $null
    try { $relative = [IO.Path]::GetRelativePath($Root, $full) } catch { $relative = [IO.Path]::GetFileName($full) }
    $item = Get-Item -LiteralPath $full -ErrorAction SilentlyContinue
    if ($null -eq $item -or -not $item.PSIsContainer) {
        if ($null -eq $item) { return [ordered]@{kind=$Kind;relativePath=$relative.Replace('\','/');bytes=0;sha256=$null;missing=$true} }
        $hash = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash
        return [ordered]@{kind=$Kind;relativePath=$relative.Replace('\','/');bytes=[Int64]$item.Length;sha256=$hash;missing=$false}
    }
    return [ordered]@{kind=$Kind;relativePath=$relative.Replace('\','/');bytes=0;sha256=$null;missing=$true}
}

function Read-LittleUInt32([byte[]]$Bytes, [int]$Offset) {
    if ($Offset -lt 0 -or $Offset + 4 -gt $Bytes.Length) { return $null }
    return [uint32](([uint32]$Bytes[$Offset]) -bor (([uint32]$Bytes[$Offset+1]) -shl 8) -bor (([uint32]$Bytes[$Offset+2]) -shl 16) -bor (([uint32]$Bytes[$Offset+3]) -shl 24))
}

function Convert-IPv4Bytes([byte[]]$Bytes, [int]$Offset) {
    if ($Offset -lt 0 -or $Offset + 4 -gt $Bytes.Length) { return '' }
    return "$($Bytes[$Offset]).$($Bytes[$Offset+1]).$($Bytes[$Offset+2]).$($Bytes[$Offset+3])"
}

function Test-CapturePcapNg {
    param([string]$Path, [string[]]$CardIPs, [int[]]$Ports)
    $result = [ordered]@{path=$Path;parseable=$false;targetPacketsPresent='unknown';targetPacketCount=0;packetInstances=0;firstTimestampNs=$null;lastTimestampNs=$null;error=$null}
    if (-not (Test-Path -LiteralPath $Path)) { $result.error='missing'; return $result }
    try { $bytes = [IO.File]::ReadAllBytes($Path) } catch { $result.error=$_.Exception.Message; return $result }
    if ($bytes.Length -lt 12) { $result.error='too_short'; return $result }
    $offset = 0; $interfaces = @{}
    while ($offset + 12 -le $bytes.Length) {
        $type = Read-LittleUInt32 $bytes $offset
        $length = Read-LittleUInt32 $bytes ($offset + 4)
        if ($null -eq $type -or $null -eq $length -or $length -lt 12 -or $offset + $length -gt $bytes.Length) { $result.error='invalid_block'; return $result }
        $endLength = Read-LittleUInt32 $bytes ($offset + $length - 4)
        if ($endLength -ne $length) { $result.error='block_length_mismatch'; return $result }
        if ($type -eq 0x00000001 -and $length -ge 20) {
            $interfaces[$interfaces.Count] = [ordered]@{linkType=[uint16](Read-LittleUInt32 $bytes ($offset+8) -band 0xffff);timestampResolution=1000000}
        } elseif ($type -eq 0x00000006 -and $length -ge 32) {
            $interfaceId = [int](Read-LittleUInt32 $bytes ($offset+8))
            $tsHigh = [uint64](Read-LittleUInt32 $bytes ($offset+12)); $tsLow = [uint64](Read-LittleUInt32 $bytes ($offset+16))
            $captured = [int](Read-LittleUInt32 $bytes ($offset+20))
            $packetOffset = $offset + 28
            if ($captured -gt 0 -and $packetOffset + $captured -le $offset + $length - 4) {
                $linkType = 1
                if ($interfaces.ContainsKey($interfaceId)) { $linkType = $interfaces[$interfaceId].linkType }
                $ipOffset = $packetOffset
                if ($linkType -eq 1 -and $captured -ge 14) {
                    if ($bytes[$packetOffset+12] -eq 8 -and $bytes[$packetOffset+13] -eq 0) { $ipOffset += 14 }
                }
                if ($ipOffset + 20 -le $bytes.Length -and $bytes[$ipOffset] -shr 4 -eq 4) {
                    $headerLength = ($bytes[$ipOffset] -band 0x0f) * 4
                    if ($bytes[$ipOffset+9] -eq 17 -and $ipOffset + $headerLength + 8 -le $bytes.Length) {
                        $source = Convert-IPv4Bytes $bytes ($ipOffset+12)
                        $destination = Convert-IPv4Bytes $bytes ($ipOffset+16)
                        $sourcePort = [int]((([int]$bytes[$ipOffset+$headerLength] -shl 8) -bor [int]$bytes[$ipOffset+$headerLength+1]))
                        $destinationPort = [int]((([int]$bytes[$ipOffset+$headerLength+2] -shl 8) -bor [int]$bytes[$ipOffset+$headerLength+3]))
                        ++$result.packetInstances
                        if (@($CardIPs) -contains $source -and @($Ports) -contains $destinationPort) {
                            ++$result.targetPacketCount
                        }
                    }
                }
            }
            $timestamp = ($tsHigh -shl 32) -bor $tsLow
            if ($null -eq $result.firstTimestampNs -or $timestamp -lt $result.firstTimestampNs) { $result.firstTimestampNs = $timestamp }
            if ($null -eq $result.lastTimestampNs -or $timestamp -gt $result.lastTimestampNs) { $result.lastTimestampNs = $timestamp }
        }
        $offset += $length
    }
    $result.parseable = $true
    $result.targetPacketsPresent = if ($result.targetPacketCount -gt 0) { $true } else { $false }
    return $result
}

function New-CaptureLock([string]$Root) {
    $path = Join-Path $Root '.capture-session.lock'
    $stream = $null
    try {
        $stream = [IO.File]::Open($path, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
        $stream.Lock(0, 1)
        return $stream
    } catch {
        if ($stream) { $stream.Dispose() }
        return $null
    }
}

function Release-CaptureLock($Stream) {
    if ($null -eq $Stream) { return }
    try { $Stream.Unlock(0, 1) } catch {}
    try { $Stream.Dispose() } catch {}
}

function Add-CaptureFailure([System.Collections.Generic.List[string]]$List, [string]$Message) {
    if (-not [string]::IsNullOrWhiteSpace($Message) -and -not $List.Contains($Message)) { $List.Add($Message) }
}
