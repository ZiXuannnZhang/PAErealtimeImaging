# R5 验收：有效载荷槽位

结论：2026-11-11 主代理独立验收通过。实施起点 2b667c07ef41524373020c669bb826a6b5b73c35，执行代理 gpt-6.1-terra / high。

SourceCore 以 min(1440, 本槽剩余采样字节) 判断最低长度。槽内载荷不足沿用 Short 决策，不写 seen 或 unique，不阻止合法同槽补包。首到包锚点、来源和 START/recent 规则保持原样。合法短末包以及末槽补齐均接受，沿用超长部分裁剪行为。只有报头而没有任何有效槽的活跃组帧仍按既有 100 ms 截止产生 partial，并与同步成像隔离；不改变 R4 策略。

分析器保留 raw 槽位、重复、sourceClosed/sourceUnique 等历史口径，另计算有效槽与缺失有效槽；短包异常仍可见，合法补齐后的组可判载荷完整。trace 的 Short 编号保持不变；样本数缺失时仍标记未验证末包长度。

独立反例：一张卡、400 点、32 bit，三槽分别为 0 / 1440 / 320 载荷字节。用同一探针编译基线与修复后的 SourceCore：基线 complete=1、sync=1、首槽有效字节 0；修复后 complete=0、sync=0。新的 UDP 生产入口用例断言纯头部 triggersComplete 为 0、triggersPartial 为 1。核心夹具另外覆盖头部、短非末/末、合法重发与重复、16/32 bit、末包 padding、包号回绕、单卡及多卡同步。

验证：Qt 6.8.0 / MinGW 13.1 / Ninja Debug。基线七项 CTest 7/7（13.12 s）；候选七项 7/7（13.44 s）：paimage_core_checks、paimage_protocol_checks、paimage_network_test、paimage_production_gap_test、paimage_socket_short、paimage_start_fence_regression、paimage_host_output_test。分析器夹具以 Python 直接运行，包含短包后同槽重发及物理轮摘要。未接现场采集卡。
