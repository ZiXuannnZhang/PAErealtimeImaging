param(
    [Parameter(Mandatory=$true)][string]$TrialId,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string[]]$CardIPs = @(),
    [int[]]$Ports = @(8000,8001,8002,8003,8004),
    [string]$ChannelDir = '',
    [int]$MaxWaitSeconds = 60,
    [string]$WprProfile = 'GeneralProfile',
    [string]$RunId = '',
    [string]$ListenId = '',
    [string]$MeasurementSessionId = '',
    [string]$CaptureSessionToken = '',
    [string]$ApplicationRequestPath = '',
    [string]$AdapterPath = '',
    [switch]$SkipWpr
)

# Compatibility entry point. The implementation and state machine live in
# start_system_capture_admin.ps1; this name remains for existing operation
# cards and scripts. The new application handshake is still mandatory.
$target = Join-Path $PSScriptRoot 'start_system_capture_admin.ps1'
$forward = @{
    OutputDirectory=$OutputDirectory; TrialId=$TrialId; Ports=$Ports; MaxWaitSeconds=$MaxWaitSeconds; WprProfile=$WprProfile
}
if ($CardIPs.Count -gt 0) { $forward.CardIPs = $CardIPs }
if (-not [string]::IsNullOrWhiteSpace($ChannelDir)) { $forward.ChannelDir = $ChannelDir }
if (-not [string]::IsNullOrWhiteSpace($RunId)) { $forward.RunId = $RunId }
if (-not [string]::IsNullOrWhiteSpace($ListenId)) { $forward.ListenId = $ListenId }
if (-not [string]::IsNullOrWhiteSpace($MeasurementSessionId)) { $forward.MeasurementSessionId = $MeasurementSessionId }
if (-not [string]::IsNullOrWhiteSpace($CaptureSessionToken)) { $forward.CaptureSessionToken = $CaptureSessionToken }
if (-not [string]::IsNullOrWhiteSpace($ApplicationRequestPath)) { $forward.ApplicationRequestPath = $ApplicationRequestPath }
if (-not [string]::IsNullOrWhiteSpace($AdapterPath)) { $forward.AdapterPath = $AdapterPath }
if ($SkipWpr) { $forward.NoWpr = $true }
& $target @forward
exit $LASTEXITCODE
