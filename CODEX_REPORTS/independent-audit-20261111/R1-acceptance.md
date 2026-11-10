# R1 验收：转换元数据一致性

结论：2026-11-11，主代理独立验收通过，准入 R2。实现基线 a284bf3625aeac57d7dd40e1a02c844b146132e3，执行代理 gpt-6.1-terra / high。

FrameConverter 在 call_once 执行者内、同一次 converter mutex 临界区冻结 normalization、saveSession 和 saveOverride。读取快照后释放锁，再做一次 raw 解码；等待 call_once 不持 converter mutex。保存覆盖应用于独立 TriggerGroup 副本。sync 重复分类时依旧合并 roundComplete，保留稳定的 isFinalLogicalTrigger。

主代理检查 SourceCore 正常运行和 startup 缓冲释放顺序：同一源对象必先经过 card，再进入 sync。HostOutput 的正常及尾帧入队路径均在发布前完成相关标记。正常同步重复分类在 normalizer 缓存窗口内，保留物理身份、逻辑序号和最终触发属性。保留 sync 原有轮次锁范围与超时屏障，不增加接收线程锁内的解码、文件写入或等待。

独立验证：Qt 6.8.0 / MinGW 13.1 / Ninja Debug，delivery/tests 单独 configure（CMAKE_OBJECT_PATH_MAX=180）；构建对应五个目标，执行：

    ctest --test-dir D:/ChatGPT/PAERealtimeImaging/artifacts/independent-audit-20261111-tests --output-on-failure -R "^(paimage_host_output_test|paimage_conversion_checks|paimage_worker_checks|physical_round_normalizer_test|count_boundary_save_binding_test)$"

5/5 通过（5.75 s），git diff --check 通过。转换测试用条件变量分别让保存和成像先取得快照，在解码前交错更新三项标记，核对只解码一次、冻结的身份及保存代、单次边界及保存覆盖隔离。既有 HostOutput / count-boundary 测试覆盖多卡、超时旧轮和按卡尾帧文件。

基线复跑：沙箱默认 TEMP 下 C2 失败；调试器确认已有 8 个成像对象但两个保存计数为 0。设置 TEMP/TMP 为工作区短路径后，未修改的基线 C2 所属目标通过（0.83 s），断言未改。并发缺陷依据同步关系确认；未运行 TSAN 或实机测试。
