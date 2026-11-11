# R2 执行记录：Ring 输入所有权和消费确认

状态：2026-11-11 主代理独立验收通过，准入 R3。实施起点为已验收 R1 的 d7f263adc5768f58ca3a329753c4562b1c1b5e15。

R2 分为共享内存协议及锁、生产者接线、服务及自检接线、跨进程故障验收。每段由 gpt-6.1-terra / high 执行，最终由主代理独立检查并构建。生产回调持有组包锁，队列入队只允许短临界区。Qt 6.8.0 的 Windows QSystemSemaphore 使用无超时等待；Ring 路径因此采用原生命名映射及有界输入、输出互斥，保留线性分支。

R2 改动前，主代理使用 MinGW 13.1 / Qt 6.8.0、标准 mingw-debug 构建 d7f263a。沙箱内 windeployqt 的子进程查询失败（3221225794）；在权限复核后的同一标准脚本成功部署 Qt，未改源码或 preset。生成的 BuildIdentity 为 d7f263adc5768f58ca3a329753c4562b1c1b5e15。此为回归基线，非交付。

独立的 delivery/tests 构建中，ring_fbp_service_test、ring_fbp_fusion_two_channel_test、ring_fbp_controller_test、ring_block_assembler_test、imaging_bypass_test、ring_production_blockers_test、ring_shm_observability_test、ring_round_identity_test 通过（8/8，17.25 s）。原有 selftest 在真实 ImagingSvc 子进程完成 10/10 块，通知/复制不匹配为 0（RTX 4060 Laptop、CUDA 12.8，两个通道）。命令：

    ring_svc_selftest.exe --svc <build/mingw_debug/bin/ImagingSvc.exe> --synthetic 1 --id 14 --channels 3 --samples 512 --grid-mm 1 --block 400 --rounds 1 --exact-fbp 1 --no-save 1 --out <短路径>

原有串行自检每次等待快照，无法证明慢消费者下的输入所有权。后续需要在生产接线中验证这一缺口。新增 RingInputTransport 的独立测试已通过，覆盖忙槽保留、复制后确认、重放、撤回、epoch 与遗弃锁恢复；不能替代真实进程验收。

实施：v3 只保留一个共享输入槽，生产 controller 的 owner 定时器推进最多 8 个待提交块。待提交原始数据（含 in-flight 与等待 owner 释放的旧 payload）受 1 GiB 总预算约束，映射及服务独立副本另计。默认 8 通道、200 A-line/通道、10,000 点约 64 MB/块。队列满只拒绝当前块并计数，已接纳的块依次推进，下一物理轮可恢复。传输失败触发有界撤销/停止；每次发布和 reset 的重试截止为 2 s，锁等待在 owner 路径上为 5 ms、服务侧为 20 ms。通知使用同一 epoch/publication ID 重试，服务获得独立输入后清 Ready 并在槽内写确认；该确认不代表重建完成。

新布局在输出区之后追加带逻辑长度的输入元数据，适配 Windows 页对齐，保留原输出偏移；v2/v3 使用不同映射名。方向、物理轮身份、块号、源轮终结与三个 payload 一同受输入锁保护。序号缺口锁存本轮不完整，允许继续部分重建，禁止错误终结。reset 的共享 epoch 可恢复单次通知或锁机会丢失；进程停止/异常退出清 owner 队列、输出 worker、映射和 socket。

在主代理的独立构建中，新版正常合成轮完成 10/10 块；主动丢中块后服务复制 9/10，出现 round_block_count_mismatch 且最终快照不完整。v2 发送端对 v3 服务返回错误 2010（协议需要 v3）；v3 发送端对 v2 服务返回 2007（旧映射不可附接），两次自检均退出 2，无静默附接。第一轮 R2 定向 CTest 13/13，通过队列满、按序确认、无 reset 的下一轮恢复、reset 后复用块序号、直接服务重建和 4000 帧下游积压下的原始保存隔离。随后完成通知失败注入、异常退出重启与 MainWindow 回归，见下文。

组合自检补充：旧块号跨普通物理轮保持单调，只在超时 reset 时归零，不能把它直接当成轮内序号。修复在 v3 槽内另带 round_block_index，生产者只保存当前轮与首次块号（在容量判断前登记），保留原 block_seq 观测。修复前第二轮最后一个块被错误判不完整；修复后丢中块的首轮报告 9/10 且不完整，下一轮 10/10 正常完成，下一轮首图与干净首图逐像素最大差为 0。自检错误路径释放仅由自检启动的服务子进程。

最新定向 CTest 14/14（29.39 s）。其中真实故障测试注入首次通知失败，检查 A 已占有的槽中数据、角度、通道与提交身份不变，B 等待；同一 ID 重试后服务按序确认；强制结束自启服务后，使用新 epoch 重启并完成新轮。断言覆盖生产控制器、生产 ImagingSvc 与 CUDA 子进程，不把复制确认当成重建完成。

实际主入口：mingw-debug 配置启用 BUILD_MAINWINDOW_START_TEST，标准主程序/ImagingSvc/selftest 编译部署通过。真实 MainWindow 启停与常驻服务回归 1/1（50.79 s）；该入口产物使用已加入轮内序号、但仍采用等价轮次查表的中间构建，随后常量空间版本已重新编译通过定向 14/14，最终组合阶段将在精确交付提交上重建主入口。未做现场四卡采集。

定向命令：对 delivery/tests 的独立 Debug Ninja 构建，先按实际 CTest 清单构建选中目标，再运行 ctest --test-dir <audit-tests> --output-on-failure -R "^(ring_input_transport_test|ring_shm_observability_test|ring_round_identity_test|ring_production_blockers_test|imaging_bypass_test|ring_fbp_controller_(test|burst_test|faults_test)|ring_block_assembler_test|ring_fbp_service_test|ring_fbp_fusion_two_channel_test|ring_timevar_service_test|ring_timevar_ui_test|data_processor_imaging_isolation_test)$"。Qt 6.8.0 / MinGW 13.1 / Ninja；QT_QPA_PLATFORM=offscreen，TEMP/TMP 为工作区短路径。
