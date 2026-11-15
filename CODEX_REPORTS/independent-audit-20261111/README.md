# 独立审核修复交付索引

修复分支：`codex/independent-audit-repair-20261111`。基线：`a284bf3625aeac57d7dd40e1a02c844b146132e3`。按基线保留分卡尾帧宽限与 `_late` 隔离；仅向修复分支交付，待用户验收。

| 项目 | 实施与独立验收 |
| --- | --- |
| R1 | [同帧转换元数据冻结](R1-acceptance.md) |
| R2 | [Ring 输入确认、限流及真实服务故障验收](R2-execution.md) |
| R3a、R3b | [A/B 保存确认、回退和异常状态](R3-acceptance.md) |
| R4 | [证据边界及保持现行 100 ms 策略](R4-evidence.md) |
| R5 | [只按有效采样载荷确认收齐](R5-acceptance.md) |
| R6 | [每轮接收预算、停止和四卡回放](R6-acceptance.md) |
| R7 | [三类 trace 的导出完整性判据](R7-acceptance.md) |
| R8 | [固定日志快照、活动轮转与清理生命期](R8-acceptance.md) |

组合验收使用独立的 delivery/tests 构建、仓库标准 mingw-debug configure/build、真实 MainWindow 启停测试和真实 ImagingSvc 子进程的合成 GPU 自检。精确提交、产物身份、依赖来源及测试日志记录在工作区的组合验收回执中。
