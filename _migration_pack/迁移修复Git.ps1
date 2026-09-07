# 迁移修复Git.ps1
# 用途：项目内容被直接复制到新工作区后，.git 目录通常不会一起复制，导致
#       "当前目录不是 Git 仓库"。本脚本用同目录下的 git bundle 恢复全部提交与分支，
#       同时保留已复制到工作区的未提交修改（不覆盖工作区文件）。
#
# 用法（在新工作区中执行）：
#   pwsh -ExecutionPolicy Bypass -File .\_migration_pack\迁移修复Git.ps1
#
# 前置条件：
#   - 本脚本位于 <新工作区>\_migration_pack\ 下；
#   - 同目录存在 realtime_imaging_migration.bundle。
# 预期结果：
#   - HEAD = 8000b763c56a2da38ee7d50877a80126c57e1e5a（master）
#   - 分支：master、backup/cf-dmas-pcf-20260816、backup/sysdelay-per-channel-20260817
#   - git status 显示与交接文档第 3.2 节一致的未提交修改/更名。

param(
    [string]$BundlePath = "",
    [string]$WorkspaceRoot = "",
    [string]$GitExe = ""
)

$ErrorActionPreference = "Stop"

if (-not $WorkspaceRoot) {
    $WorkspaceRoot = Split-Path -Parent $PSScriptRoot
}
if (-not $BundlePath) {
    $BundlePath = Join-Path $PSScriptRoot "realtime_imaging_migration.bundle"
}

Write-Host "== Git repository repair =="
Write-Host "WorkspaceRoot : $WorkspaceRoot"
Write-Host "BundlePath    : $BundlePath"

if (-not (Test-Path -LiteralPath $WorkspaceRoot)) {
    throw "WorkspaceRoot does not exist: $WorkspaceRoot"
}
if (-not (Test-Path -LiteralPath $BundlePath)) {
    throw "Bundle file not found: $BundlePath. Put it next to this script first."
}

# 优先用显式传入的 git，其次工作区便携版 git，最后 PATH 中的 git
if ($GitExe) {
    $git = $GitExe
} else {
    $portableGit = Join-Path $WorkspaceRoot "_tools\git\cmd\git.exe"
    if (Test-Path -LiteralPath $portableGit) {
        $git = $portableGit
    } else {
        $probe = Get-Command git -ErrorAction SilentlyContinue
        if ($probe) { $git = $probe.Source } else { $git = $null }
    }
}
if (-not $git) {
    throw "No git found. Copy the original workspace's `_tools\git` folder to the new workspace, or pass -GitExe <path to git.exe>."
}
Write-Host "Using git      : $git"

function Invoke-Git {
    param([string[]]$GitArgs)
    $out = & $git -C $WorkspaceRoot @GitArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "git $($GitArgs -join ' ') failed:`n$($out -join "`n")"
    }
    return $out
}

if (Test-Path -LiteralPath (Join-Path $WorkspaceRoot ".git")) {
    Write-Host ".git already exists; skip init, only verify below."
} else {
    Write-Host ".git missing -> initializing from bundle (keeps working tree intact)."
    Invoke-Git @("init") | Out-Null

    # 新 init 的仓库当前分支处于未出生状态，不能直接 fetch 到已检出的
    # refs/heads/*（git 会拒绝）。先 fetch 到 remotes/origin/*，再手动建立本地分支。
    Invoke-Git @("remote", "add", "origin", $BundlePath) | Out-Null
    Invoke-Git @("fetch", "origin", "+refs/heads/*:refs/remotes/origin/*") | Out-Null

    $branchMap = @(
        @{ Local = "refs/heads/master"; Remote = "refs/remotes/origin/master" },
        @{ Local = "refs/heads/backup/cf-dmas-pcf-20260816"; Remote = "refs/remotes/origin/backup/cf-dmas-pcf-20260816" },
        @{ Local = "refs/heads/backup/sysdelay-per-channel-20260817"; Remote = "refs/remotes/origin/backup/sysdelay-per-channel-20260817" }
    )
    foreach ($b in $branchMap) {
        Invoke-Git @("update-ref", $b.Local, $b.Remote) | Out-Null
    }

    # mixed reset：把 HEAD/索引指向 master 提交，但不改动工作区文件，
    # 已复制的未提交改动会原样保留为 modified/deleted/untracked。
    Invoke-Git @("reset", "master") | Out-Null
}

Write-Host "`n== Verification =="
$head = (& $git -C $WorkspaceRoot rev-parse HEAD 2>&1).ToString().Trim()
Write-Host "HEAD = $head"
$expected = "8000b763c56a2da38ee7d50877a80126c57e1e5a"
if ($head -eq $expected) {
    Write-Host "HEAD matches expected master commit (8000b76) : OK"
} else {
    Write-Warning "HEAD != expected ($expected). Check bundle completeness before continuing."
}

Write-Host "`nBranches:"
& $git -C $WorkspaceRoot branch -a

Write-Host "`ngit status --short (expected: 12 modified tracked files + Handoff/HANDOFF rename D/?? pairs + untracked data):"
& $git -C $WorkspaceRoot status --short

Write-Host "`nDone. Next step: run .\_migration_pack\迁移重建构建缓存.ps1 to regenerate path-dependent CMake caches."
