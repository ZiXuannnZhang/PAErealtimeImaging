# ChatGPT 网页端 + Codex Desktop GitHub 双端联动快速上手

## 结论

本次已验证以下链路可用：

~~~
网页端 ChatGPT
  → GitHub 连接
  → 读取 ZiXuannnZhang/PAErealtimeImaging 私有仓库

Codex Desktop
  → Windows PowerShell
  → Git for Windows
  → 仓库级 SSH Deploy Key
  → GitHub SSH over 443
  → 本地 commit
  → 推送测试分支
~~~

验证时间：2026-09-07。

目标仓库：ZiXuannnZhang/PAErealtimeImaging

默认分支：main

README 已确认完整内容为：

~~~
# PAErealtimeImaging
~~~

## 已完成的端到端验证

桌面端 Codex 使用身份：

~~~
laptop-buqlmv81\codexsandboxonline
~~~

工具版本：

~~~
Git 2.55.0.windows.5
OpenSSH_for_Windows_9.5p2
Git Credential Manager 2.9.1
~~~

由于 Codex sandbox 与真实 Windows 用户隔离，GCM/Windows Credential Manager 不适合作为无人值守认证方式。本次改用目标仓库专用 Deploy Key，并通过 GitHub 官方 SSH 443 端口连接：

~~~
ssh.github.com:443
~~~

测试分支：

~~~
codex/ssh-unattended-test
~~~

测试文件：

~~~
codex-git-push-test.txt
~~~

文件完整内容：

~~~
双端联动测试696202
~~~

本地 commit SHA：

~~~
7e53d33bb50e160862eac91e08a45799f7c33d6d
~~~

远端测试分支 SHA 与本地 SHA 一致，比较结果为 True。测试分支已成功推送，main 未修改、未 merge。

## 持久化 SSH 配置

仓库专用 SSH 文件目录：

~~~
C:\Users\yyps\.codex\ssh\PAErealtimeImaging
~~~

文件用途：

~~~
deploy_ed25519       私钥，禁止显示、复制、提交或发送
deploy_ed25519.pub   公钥
github_known_hosts   固定的 GitHub host key
~~~

公钥已经添加到：

~~~
ZiXuannnZhang/PAErealtimeImaging
  → Settings
  → Deploy keys
~~~

并已开启 Allow write access。这是仓库级权限，不是账号级 SSH key。

github_known_hosts 使用固定 host key，指纹为：

~~~
SHA256:+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU
~~~

测试仓库的 .git/config 已保存 repository-local core.sshCommand，包括：

~~~
-i C:/Users/yyps/.codex/ssh/PAErealtimeImaging/deploy_ed25519
-o IdentitiesOnly=yes
-o UserKnownHostsFile=C:/Users/yyps/.codex/ssh/PAErealtimeImaging/github_known_hosts
-o StrictHostKeyChecking=yes
-o HostName=ssh.github.com
-p 443
-o KexAlgorithms=curve25519-sha256
~~~

因此后续在同一 checkout 中不需要设置临时 GIT_SSH_COMMAND。

## 其他工作会话快速接入

目标工作区是：

~~~
D:\ChatGPT\PAERealtimeImaging
~~~

本次验证使用的临时测试 checkout 是：

~~~
C:\Users\yyps\Documents\Codex\2026-09-06\yue\PAErealtimeImaging
~~~

两者是不同的本地 checkout。其他工作会话若使用 D:\ChatGPT\PAERealtimeImaging，首次在该仓库执行以下配置：

先确认当前环境与 remote：

~~~powershell
whoami
git --version
ssh -V
git remote -v
git status
~~~

正常情况下，whoami 应为 laptop-buqlmv81\codexsandboxonline。

先读取 origin：

~~~powershell
git remote get-url origin
~~~

origin 应为：

~~~
git@github.com:ZiXuannnZhang/PAErealtimeImaging.git
~~~

如果当前 origin 不是该 SSH URL，而是 HTTPS 或其他地址，则先改为：

~~~powershell
git remote set-url origin git@github.com:ZiXuannnZhang/PAErealtimeImaging.git
~~~

然后配置 repository-local SSH：

~~~powershell
$SshDir = "C:\Users\yyps\.codex\ssh\PAErealtimeImaging"
$KeyPosix = (Join-Path $SshDir "deploy_ed25519").Replace("\","/")
$KnownHostsPosix = (Join-Path $SshDir "github_known_hosts").Replace("\","/")
$SshCommand = "ssh -i $KeyPosix -o IdentitiesOnly=yes -o UserKnownHostsFile=$KnownHostsPosix -o StrictHostKeyChecking=yes -o HostName=ssh.github.com -p 443 -o KexAlgorithms=curve25519-sha256"
git config --local core.sshCommand $SshCommand
git fetch origin
~~~

检查 repository-local commit identity：

~~~powershell
git config --local user.name
git config --local user.email
~~~

如果任一项为空，则只在当前仓库设置：

~~~powershell
git config --local user.name "Codex Desktop"
git config --local user.email "codex-desktop@local.invalid"
~~~

不要修改全局 Git identity。

## 推荐的无人值守工作流

每次开始工作：

~~~powershell
Set-Location "D:\ChatGPT\PAERealtimeImaging"
git fetch origin
git switch main
git merge --ff-only origin/main
git status
~~~

如果 `git merge --ff-only origin/main` 失败，停止并检查本地 main 与远端 main 的分叉原因，不要自动 reset 或 force。

新任务使用独立分支，并明确从最新 main 创建，不直接修改 main：

~~~powershell
git switch -c <task-branch>
~~~

提交前必须检查：

~~~powershell
git status
git diff
git add -- <明确的文件路径>
git status
git diff --cached
~~~

确认 staged diff 只有预期文件后：

~~~powershell
git commit -m "<commit message>"
git push -u origin <task-branch>
~~~

推送后核验：

~~~powershell
$LocalSha = git rev-parse HEAD
$RemoteSha = (git ls-remote origin "refs/heads/<task-branch>").Split()[0]
$LocalSha
$RemoteSha
$LocalSha -eq $RemoteSha
~~~

## 不要重复踩的坑

1. 不要依赖 GCM、Windows Credential Manager、PAT 或 git credential-manager github login。
2. 不要把私钥放进仓库、临时目录或 .cache\codex-runtimes。
3. 不要使用 StrictHostKeyChecking=accept-new。
4. SSH 22 端口在本环境 TCP 探测虽然可连，但 SSH 握手会被关闭；固定使用 ssh.github.com:443。
5. 不要把公钥添加到个人账号的 SSH and GPG keys；必须使用目标仓库的 Deploy keys。
6. 不要执行 git push --force、git push -f，也不要直接 push main。
7. 提权 shell 的身份可能是 yyps，不能用它代替 codexsandboxonline 验证无人值守链路；最终 fetch/push 应在普通 Codex sandbox 身份下验证。
8. 如果出现 UNPROTECTED PRIVATE KEY FILE，先检查私钥所有者和 ACL，不要重新生成 key。

## 当前验收状态

~~~
Deploy Key auth: 成功
SSH over 443: 成功
SSH clone: 成功
Repository-local SSH config: 成功
git fetch: 成功
Local commit: 成功
git push: 成功
Local SHA = Remote SHA: True
main: 未修改
~~~
