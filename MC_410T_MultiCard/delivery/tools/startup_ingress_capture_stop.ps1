param(
    [Parameter(Mandatory=$true)][string]$StatePath,
    [string]$AdapterPath = ''
)

# Compatibility entry point for the new owned-session stop/convert/validate
# path. It is idempotent for an already finalized state.
$target = Join-Path $PSScriptRoot 'stop_system_capture_admin.ps1'
$forward = @('-StatePath',$StatePath)
if (-not [string]::IsNullOrWhiteSpace($AdapterPath)) { $forward += @('-AdapterPath',$AdapterPath) }
& $target @forward
exit $LASTEXITCODE
