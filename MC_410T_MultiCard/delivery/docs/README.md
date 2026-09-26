# delivery/docs — 当前文档索引

本目录只保留**仍可直接指导 canonical main 开发、构建或运行**的模块文档。

## 当前文档

| 文档 | 作用 |
| --- | --- |
| `诊断日志导出.md` | 导出诊断 ZIP、证据边界和基本判读 |
| `ZeroMQ依赖说明.md` | 当前 third_party ZeroMQ runtime / import library 与重建边界 |
| `环形重建增强.md` | 未验收的环形 H(f)/双极补偿实现边界、参数与验证要求 |

相关文档位于其他目录：

- `../README.md`：当前生产采集/成像总览；
- `../tools/paimage-trace-schema.md`：PAimage trace schema；
- `../tools/startup-looplog-schema.md`：startup loop-log schema；
- `../src/RingRecon/README.md`：CPU reference；
- `../src/RingReconCuda/README.md`：CUDA rebuild / verification；
- 仓库根 `BUILD_STANDARD.md`：正式构建与交付唯一标准。

## 历史文档

旧 M2/M3 里程碑、模拟采集验收、实时成像/保存隔离的阶段测试文档已经归档到
`CODEX_REPORTS/`。这些材料仍可用于追溯设计动机和历史测试，但不代表当前 production owner、
当前 executable 名称或当前硬件验收状态。
