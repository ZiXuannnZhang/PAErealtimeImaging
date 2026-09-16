param(
    [Parameter(Mandatory=$true)][ValidateSet('check','start','status','stop','convert')] [string]$Action,
    [Parameter(Mandatory=$true)] [string]$OutputDirectory,
    [string]$LocalIP = '',
    [string[]]$CardIPs = @(),
    [string]$TrialId = ''
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $root -Force | Out-Null
$manifestPath = Join-Path $root 'receiver-capture-manifest.json'
$result = [ordered]@{
    schemaVersion=1; candidate='quick-iteration'; action=$Action; trialId=$TrialId
    capturedAt=(Get-Date).ToString('o'); status='unknown'; outputDirectory=$root
    localIP=$LocalIP; cardIPs=$CardIPs; commands=@(); limitations=@(
        'system capture start/stop is disabled in this candidate',
        'application timing remains available without administrator privileges',
        'ETL conversion does not prove capture completeness or event coverage')
}
function Invoke-ReadOnlyTool([string]$Program,[string[]]$ToolArgs) {
    $text = & $Program @ToolArgs 2>&1 | Out-String
    return [ordered]@{command=$Program+' '+($ToolArgs -join ' ');exitCode=$LASTEXITCODE;output=$text}
}
try {
    if ($Action -eq 'check') {
        $result.pktmonVersion=Invoke-ReadOnlyTool 'pktmon.exe' @('help')
        $result.pktmonFilterHelp=Invoke-ReadOnlyTool 'pktmon.exe' @('filter','add','help')
        $result.pktmonStartHelp=Invoke-ReadOnlyTool 'pktmon.exe' @('start','help')
        $result.pktmonStatus=Invoke-ReadOnlyTool 'pktmon.exe' @('status')
        $profile=(Join-Path $PSScriptRoot 'receiver-scheduling.wprp')+'!Receiver'
        $result.wprProfile=Invoke-ReadOnlyTool 'wpr.exe' @('-profiledetails',$profile)
        $result.wprStatus=Invoke-ReadOnlyTool 'wpr.exe' @('-status')
        $result.status=if($result.pktmonStatus.exitCode -eq 0){'tools-inspected'}else{'permission-required-for-pktmon-status'}
    } elseif ($Action -eq 'convert') {
        $etl=Join-Path $root 'receiver-pktmon.etl'
        if(-not (Test-Path -LiteralPath $etl)){throw 'receiver-pktmon.etl is missing'}
        $result.commands += Invoke-ReadOnlyTool 'pktmon.exe' @('etl2txt',$etl,'--out',(Join-Path $root 'receiver-pktmon.txt'))
        $result.commands += Invoke-ReadOnlyTool 'pktmon.exe' @('etl2pcap',$etl,'--out',(Join-Path $root 'receiver-pktmon.pcapng'))
        $result.commands += Invoke-ReadOnlyTool 'pktmon.exe' @('etl2pcap',$etl,'--drop-only','--out',(Join-Path $root 'receiver-pktmon-drops.pcapng'))
        $result.status=if(@($result.commands|Where-Object exitCode -ne 0).Count){'conversion-incomplete'}else{'converted-unvalidated-input'}
    } else {
        $result.status='disabled-in-quick-candidate'
        $result.error='Safe ownership, bounded automatic stop and ETL event validation are pending; no system session was changed.'
    }
} catch {
    $result.status='unknown-or-failed';$result.error=$_.Exception.Message
}
$result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath -Encoding utf8
$result | ConvertTo-Json -Depth 12
if($result.status -in @('unknown-or-failed','conversion-incomplete')){exit 2}
