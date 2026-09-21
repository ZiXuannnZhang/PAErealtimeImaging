# 独立 Frontend Preprocessing Stage 架构落地

发布时间：2026-09-22 03:12（UTC+8）
任务类型：IMPLEMENTATION

## 1. Task

在当前全分辨率显示基础上落地独立、异步的 Frontend Preprocessing Stage。

本任务只实现架构与 identity processing，不实现 Butterworth/SOS/forward-backward filtering，不增加滤波参数 UI。

目标数据链：

~~~text
raw TriggerGroup
├─ FileSaver / raw save          -> raw，不经过 frontend stage
├─ FramePublisher                -> raw，不经过 frontend stage
└─ FrontendPreprocessor::submit(raw const)
      ↓ bounded FIFO / worker
      ↓ deep-copy frontend-owned TriggerGroup
      ↓ processFrontendSignal()  -> 本任务 identity
      ↓ full-resolution display preparation
      ├─ DisplayBuffer
      └─ existing RingFeedSink / ImagingBypass
~~~

Display 与 Ring 必须消费同一次 preprocessing 产生的 frontend-owned 数据。

## 2. Exact branch / starting SHA

实现分支：

~~~text
codex/frontend-preprocessor-stage-20260922
~~~

精确起点：

~~~text
9938269220fcaf8e81216820f6bb46549ca59888
~~~

开始前：

~~~powershell
git fetch --prune origin
git show origin/main:BUILD_STANDARD.md
git show origin/codex/task-docs:TASKS/独立FrontendPreprocessingStage架构落地_20260922-031200.md
git switch codex/frontend-preprocessor-stage-20260922
git merge --ff-only origin/codex/frontend-preprocessor-stage-20260922
git rev-parse HEAD
git status --porcelain --untracked-files=no
~~~

要求 HEAD 精确等于上述起点且 tracked clean。若不满足，停止并报告；不得 reset/rebase/force。

不得 merge/rebase/cherry-pick main 或其他分支进入本任务。

## 3. Architecture requirements

### 3.1 Ownership

- FrontendPreprocessor 输入必须视为 const raw TriggerGroup。
- worker 中创建独立 deep copy；不得原地修改输入对象。
- frontend clone 必须保留完整 metadata 与 full-resolution freqA/freqB，包括 measurementSession、roundGeneration、triggerSeq、logicalTriggerIndex、roundComplete、isFinalLogicalTrigger 等现有字段。
- raw save 路径和 FramePublisher 继续使用原 raw group。
- 本任务完成后 DataProcessor 不得再直接修改 shared raw group 的 *_display 字段。

### 3.2 Per-card async stage

- 每张采集卡一个 FrontendPreprocessor worker instance。
- 生命周期由 NetworkController 管理。
- 每卡 stage 使用有界 FIFO，保持该卡 trigger 顺序；禁止 latest-only queue。
- submit 必须 non-blocking；HostOutput/DataProcessor 热路径只做 enqueue，不执行 deep copy、display preparation 或 Ring consumer。
- deep copy 与所有 frontend work 必须发生在 worker thread。

### 3.3 Processing order

worker 内顺序冻结为：

~~~text
dequeue raw const
→ stale check
→ deep copy
→ processFrontendSignal(frontend clone)   // identity in Task 1
→ full-resolution display preparation
→ stale check again
→ DisplayBuffer update / updateFullRes
→ RingFeedSink(frontend clone)
~~~

第二次 stale check 必须存在，避免 frame 在 worker 已取出后遇到 session/timeout boundary 仍被分发。

processFrontendSignal() 必须形成清晰的单一插入点，后续高低通滤波只在这里作用一次；本任务实现必须是 identity。

### 3.4 DataProcessor boundary

- DataProcessor 的 frontend 路径改为调用 FrontendPreprocessor submit sink。
- 将现有 full-resolution display preparation 从 DataProcessor 移到 frontend stage。
- DataProcessor 不再直接拥有/调用 DisplayBuffer 和 RingFeedSink 的 frontend 分发职责；如为兼容历史测试保留 API，生产路径必须只有 FrontendPreprocessor 为 owner。
- FramePublisher 仍在 frontend submit 之前消费 raw group。
- legacy save + displayAndRing 同次 deliverAssembled 路径也必须保持 raw save 与 frontend submit 隔离。

### 3.5 Async result semantics

现有 DeliveryResult 的同步 imagingAccepted/imagingDropReason 语义不能继续代表异步 Ring 最终结果。

实现必须：

- 为 frontend enqueue 定义独立 submit result / accepted 状态；
- HostOutput stage-7 trace 改为记录 frontend enqueue 结果，而不是伪装为最终 Ring 结果；
- Ring 最终 Accepted/QueueFull/Busy/Disabled 等继续由现有 ImagingBypass/Ring 统计负责；
- frontend queue rejection 不得计入 UDP packet loss、missingTriggerCount、saveQueueDiscards。

不得复用 ImagingSubmitResult 作为 frontend queue 的语义类型。

### 3.6 Session / timeout stale barrier

FrontendPreprocessor 必须有明确 session/boundary lifecycle。

至少满足：

- beginSession(newSession) 后旧 session pending/in-flight frame 不得再进入 Display/Ring；
- measurement stop/disarm 时 pending frontend work 被清理或失效，不得 stop 后继续分发旧 frame；
- PhysicalRound TimeoutBoundary 必须推进 frontend stale barrier，旧 roundGeneration 的 queued 或 in-flight frame 在 dispatch 前被丢弃；
- CountBoundary 不得错误丢弃刚完成的 final logical trigger；该 final frame 必须仍能正常分发一次。

可选择具体 API 名称，但生产接线必须复用现有 measurement session 和 PhysicalRoundNormalizer boundary 事实来源，不允许另造独立轮次推断。

### 3.7 Lifecycle / destruction

- stage 必须在可能提交 frontend frame 之前启动。
- 停止监听时先停止上游提交，再停止/清空 stage，然后才能销毁 DisplayBuffer/Ring sink 依赖。
- rollback/start failure/destructor 路径同样必须安全 join/clear，不得留下悬空 worker。
- 不允许使用 terminate 作为 FrontendPreprocessor 正常停止机制。

## 4. Stage observability

FrontendPreprocessor 至少提供线程安全 Snapshot，用于测试和后续性能任务。

至少包含：

~~~text
accepted/enqueued
processed
queueRejected
staleDropped
exceptionDropped
currentDepth
maxDepth
~~~

如统计 downstream Ring result，必须明确标记为 downstream result，不能混入 ingress/save loss。

NetworkController 应提供按 card 查询 frontend snapshot 的最小接口；本任务不新增 UI 展示。

## 5. Future filter configuration contract

后续滤波器参数定义已冻结为：

~~~text
high-pass cutoff frequency : adjustable
high-pass order            : adjustable
low-pass cutoff frequency  : adjustable
low-pass order             : adjustable
sample rate                : production FPGA_ADC_FREQ_HZ = 250 MHz
~~~

未来用户输入位置：环形扫描模式的“成像参数”弹窗 RingConfigDialog。

本任务要求：

- 不新增这四个 UI 控件；
- 不实现参数持久化；
- 不实现参数合法性校验；
- 不把上述参数塞进 RingReconCudaConfig 或 ImagingSvc reconstruction config；
- stage 架构不得阻碍后续在不重建采集链的情况下更新 frontend processing config。

这些参数属于 Frontend Preprocessing 配置；RingConfigDialog 只是后续 UI 输入入口。

## 6. Prohibited scope

本任务禁止：

- 实现 Butterworth coefficient design；
- 实现 SOS biquad；
- 实现 forward/backward / filtfilt；
- 引入 MATLAB historical crop/system_delay 规则；
- 修改线性成像旧滤波器；
- 修改 Ring reconstruction algorithm；
- 修改保存文件格式或 raw save 内容；
- 重新加入 display downsampling；
- 增加滤波 UI 或 QSettings key；
- 性能调优/GPU 化；
- 无关重构。

## 7. Required tests

新增独立 FrontendPreprocessor 测试，至少覆盖：

1. Identity deep-copy：输入与输出指针不同，metadata/freqA/freqB 完整一致；raw object 保持不变。
2. Full-resolution：使用 >1000 点输入，frontend/display 输出保持全部采样点。
3. One result / two consumers：Display 与 Ring 来源于同一次 frontend clone；不得分别创建独立 preprocessing 结果。
4. FIFO ordering：同卡多 trigger 顺序不改变。
5. Non-blocking submit：worker 被测试 seam 阻塞时，submit/deliverAssembled 仍快速返回，不等待 worker。
6. Bounded overflow：小容量队列可确定性触发 queueRejected；该结果与 ImagingSubmitResult 分离。
7. Save isolation：frontend queue blocked/full/rejected 时 raw save sink 仍按原语义成功；raw saved payload 不因 frontend stage 改写。
8. Session stale：切换 measurementSession 后旧 queued/in-flight frame 不得 dispatch。
9. Timeout stale：推进 timeout round barrier 后旧 round queued/in-flight frame 被 staleDropped，新 round 正常通过。
10. CountBoundary final：final logical trigger 不被 boundary barrier 提前丢弃，且只分发一次。
11. Stop lifecycle：stop/clear 后无 post-stop dispatch，worker 可确定性 join。
12. Exception containment：frontend processing/downstream callback exception 不得逃逸并破坏采集/save thread。

同时更新受接口变化影响的现有 DataProcessor/HostOutput tests。

## 8. Required regression

实现完成后按 BUILD_STANDARD 在最终 production/test source commit 上重新 configure + build。

至少运行以下 targeted tests（名称按最终 CMake 实际注册为准）：

~~~text
new frontend_preprocessor test(s)
paimage_host_output_test
data_processor_imaging_isolation_test
data_processor_batch_test
imaging_bypass_test
ring_block_assembler_test
ring_round_identity_test
session_boundary_test
session_boundary_receiver_test
filesaver_round_boundary_test
count_boundary_save_binding_test
timeout_presentation_test
~~~

然后运行完整 ctest -N 和 ctest --output-on-failure -j 4。

所有实际结果按 PASS/FAIL/SKIP/NOT_RUN 报告；失败项只做一次必要诊断，不允许无限重跑或顺手修改无关模块。

## 9. Build / evidence workflow

允许在本分支形成一个或多个实现提交，但最终必须明确：

~~~text
IMPLEMENTATION_SOURCE_SHA = 最后一个 production/test/build-input commit
REPORT_HEAD               = evidence-only receipt commit
~~~

在 IMPLEMENTATION_SOURCE_SHA tracked-clean 状态执行标准 Windows mingw-debug configure/build 和 tests。

验证后将文本证据提交到：

~~~text
CODEX_REPORTS/frontend-preprocessor-stage-20260922/
~~~

至少包含：

~~~text
validation-receipt.md
build-summary.txt
ctest-targeted.txt
ctest-full.txt
git-receipt.txt
~~~

REPORT_HEAD 相对 IMPLEMENTATION_SOURCE_SHA 的 diff 只能包含该 CODEX_REPORTS 目录。

不得提交 build tree、exe/dll/zip。

## 10. Push

完成后：

~~~powershell
git push origin HEAD:refs/heads/codex/frontend-preprocessor-stage-20260922
git fetch origin refs/heads/codex/frontend-preprocessor-stage-20260922:refs/remotes/origin/codex/frontend-preprocessor-stage-20260922
git rev-parse HEAD
git rev-parse origin/codex/frontend-preprocessor-stage-20260922
git ls-remote origin refs/heads/codex/frontend-preprocessor-stage-20260922
~~~

三处最终 SHA 必须一致。禁止 force push。

## 11. Required final report

只返回本任务执行事实：

~~~text
Implementation branch
Starting SHA
IMPLEMENTATION_SOURCE_SHA
REPORT_HEAD
Changed production/test files

FrontendPreprocessor ownership/lifecycle summary
Queue capacity/policy
Session + timeout stale barrier implementation point
HostOutput/DataProcessor async result semantic changes
Frontend Snapshot fields

Build result + BuildIdentity
Targeted tests: each result
Full CTest total / PASS / FAIL / SKIP / NOT_RUN

Evidence directory
Receipt-only diff result
Local HEAD
Remote HEAD
ls-remote HEAD
~~~

不要实现或报告滤波器数值结果；该部分不属于 Task 1。