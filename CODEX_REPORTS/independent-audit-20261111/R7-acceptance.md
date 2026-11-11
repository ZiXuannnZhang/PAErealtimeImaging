# R7 验收：诊断导出的完整性标记

2026-11-11。实施起点 `45a7509e37f5c4597387afdd585504169c7c93b6`，执行代理 gpt-6.1-terra / high。改动位于 `TraceBundle.cpp` 和 `paimage_trace_bundle_test.cpp`。

trace、timing、looplog 各自保留格式错误、flush 结果、边界计数覆盖和历史异常。计数覆盖包含被时间窗排除的记录；无已知边界时写 null。各流独立形成 incomplete，再汇总到 run 和 bundle。一个记录都未选中而存在损坏、已知缺失或其它不完整证据的 run 进入 skippedRuns；有完整记录的其它流仍可导出。只写完整记录，残尾不进入 ZIP。

正常部分裁剪保留 trace 的 exportClipped 与既有 incomplete 语义，format=false；健康记录全部在窗外时不进入清单。禁用 trace 不影响正常 looplog，实际发现 trace 格式损坏仍报告。缺时间锚点继续跳过时窗转换，有历史损坏或记录证据时在清单列明。live cut 继续按捕获的序号过滤，已知历史边界同样用于各流筛选。

独立反证：将新增 ZIP 回归夹具链接到起点提交的 TraceBundle.cpp，旧实现断言失败（退出 1）。直接读取旧 ZIP，坏 trace 的 traceIncomplete=false、坏 looplog 的 loopLogIncomplete=false，两者均导出 1 条完整记录；仅残尾的 run 没有出现在 skippedRuns。timing 在原组合夹具中已标异常，不能据此推断旧 bundle 总体标记总为 false。

独立复验使用 Qt 6.8.0 / MinGW 13.1 / Ninja Debug。构建并运行：

    ctest --test-dir <audit-tests> --output-on-failure -R "^(diagnostic_recorder_test|diagnostic_time_window_test|paimage_trace_bundle_test|startup_diagnostics_analyze_test)$"

ZIP 回归检查三流摘要及记录内容、无完整记录和无锚点的清单、健康 looplog、完整 bundle、部分裁剪及健康 run 全部在窗外。主代理独立验收 4/4 通过（3.02 s）。
