# R3a/R3b 验收：保存批次确认与回退

结论：2026-11-11 主代理独立验收通过。实施起点 45c3b5291722de63aba8c06cbe6b482fba5d2162，执行代理 gpt-6.1-terra / high。

FileSaver 继续按批次转换和写入。A、B 两侧 write 均返回预期长度且两侧 Qt flush 成功后，推进各自确认的文件位置和 savedCount；acceptedCount 表示进入内存批次的触发，等待消费者的测试使用该计数，保留原有磁盘长度、内容及记录归属断言。该确认不要求断电持久化。

短写或 flush 失败时，先关闭原句柄，防止 Qt 在截断后补写残留缓冲；之后用 ExistingOnly 打开新句柄，分别检查文件已确认边界、resize 及关闭结果。两侧均成功才报告已回退。任一侧失败时列受影响路径和文件完整性未知，停保存，只保留此前双侧确认的计数。Qt 6.8 的 [QFileDevice::close 源码](https://github.com/qt/qtbase/blob/v6.8.0/src/corelib/io/qfiledevice.cpp) 会在关闭时尝试 flush，故截断必须在旧句柄关闭之后。

自动保存代、容量、物理轮轮转/封存、主动 close、停止、线程退出和异常路径传播关闭结果；析构失败记录警告。_late 独立路径沿用同样的批次确认，错误带隔离文件详情传给父 saver；主文件若有独立待写批次，按正常关闭流程处理。NewOnly 及文件记录格式保留。

独立验收：基线同四项目标 4/4（6.34 s）；实施后 paimage_host_output_test、filesaver_round_boundary_test、auto_save_round_coordinator_test、count_boundary_save_binding_test 4/4（5.68 s）；补最后的 _late 双侧内容边界断言后重编对应目标，1/1（2.61 s）。使用 Qt 6.8.0 / MinGW 13.1 / Ninja Debug，QT_QPA_PLATFORM=offscreen，TEMP/TMP 为工作区短路径，命令：

    ctest --test-dir <audit-tests> --output-on-failure -R "^(filesaver_round_boundary_test|count_boundary_save_binding_test|auto_save_round_coordinator_test|paimage_host_output_test)$"

针对性夹具真实写入奇数长度前缀，模拟 Qt 尚未 flush 的失败和单侧 resize 不生效；检查另一侧逐字节保留此前记录、失败侧保留异常尾、无误报回退、停保存及单次提示。另覆盖小批和单通道 20,000 字节批次、六条关闭/轮转入口及 _late 的实际文件。未使用现场介质故障或断电测试。
