param([string]$Worktree = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'run_system_capture_synthetic.ps1') -Worktree $Worktree -NoWpr -CaseName 'no-wpr'
exit $LASTEXITCODE
