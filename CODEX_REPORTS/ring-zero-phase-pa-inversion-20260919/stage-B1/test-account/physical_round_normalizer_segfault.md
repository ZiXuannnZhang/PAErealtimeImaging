# physical_round_normalizer_test 偶发 SEGFAULT — B1 收口 S4 崩溃证据与定位记录

日期：2026-09-20（B1 收口任务）
被测源：`src/PaimageAcquisition/PhysicalRoundNormalizer.cpp`（本次任务**零改动**；与 777df4f/45cd050 相同）
测试源：`tests/paimage_core/physical_round_normalizer_test.cpp`（本次任务**零改动**）

## 1. 崩溃事实（本机提交 d8dd3daf 构建，mingw-debug）

| 运行方式 | 样本 | 通过 | 崩溃 | 崩溃率 |
|---|---|---|---|---|
| ctest 逐项单独运行 | 8 | 7 | 1（run7，SEGFAULT 0.09s） | 12.5% |
| 直接运行 exe（无 ctest/gdb） | 100 | 78 | 22 | 22% |
| 直接运行 exe（第二批） | 40 | 22 | 18 | 45% |

- ctest run7 原始记录：`prn_segfault_ctest_run7.log`（0% tests passed, SEGFAULT 0.09s）
- 无输出即崩溃（stdout/stderr 均为空文件，100/40 样本中 `crash-with-full-output=0`）→ 崩溃发生在首个 `require` 断言之前或输出缓冲未刷新处。

## 2. gdb 栈回溯（三份独立捕获）

`prn_segfault_gdb_backtrace.log`（run1）、`prn_segfault_gdb_run2.log`、`prn_segfault_gdb_run3.log`：

```
Thread 1 received signal SIGSEGV
#0 std::_Deque_iterator<paimage::PhysicalRoundNormalizer::CachedDecision...>::operator--
     stl_deque.h:215  (_M_cur = _M_last；部分样本 this=0x48——明显被内联优化扭曲)
#1/#2 std::_Deque_iterator::_M_set_node / _S_buffer_size   stl_deque.h:267/132
#3 std::_Deque_iterator::operator--                        stl_deque.h:214
#4 std::reverse_iterator<_Deque_iterator>::operator++      stl_iterator.h:298
#5 paimage::PhysicalRoundNormalizer::classify(session=7000, triggerSeq=<随机>, ns=0)
     PhysicalRoundNormalizer.cpp:141-142（recentDecisions_ 逆向扫描循环）
#6 tests/.../physical_round_normalizer_test.cpp:20
#7 tests/.../physical_round_normalizer_test.cpp:64（T1 4000 触发序列主循环）
```

- 崩溃触发点随机分布于 `trigger ∈ {2318, 2489, 2560, 3028, 3144, 3435, 3725, 3917}`——均在 T1 "4000 触发序列" 循环中段。
- 同一 exe 直接运行（不经 ctest/gdb）同样崩溃 → 排除 ctest/gdb 包装机制。
- 测试与被测源单线程（无 std::thread）；classify 内部全程持锁。

## 3. 排除与线索

已排除：
- 本任务改动：PhysicalRoundNormalizer.cpp/.h、测试源码、paimage_core CMake 均零改动（`git diff 777df4f..d8dd3da -- <files>` 为空）。
- 并发：测试单线程；normalizer 内部 mutex 正确。
- 测试逻辑改动：无。
- 环境性（ctest 并发资源）：单独逐项运行同样崩溃。

线索（未定论）：
- 崩溃点在 MinGW-w64 GCC 13.1.0 (posix-seh) libstdc++ 的 `std::deque` 反向迭代器跨 bucket（`_M_set_node`）路径。
- 该测试对同一 normalizer 连续 push/scan 4001 次（deque 规模 ≤ 8192 容量，trim 恒不触发），逻辑上无任何并发/越界路径；编译器为同版本，源码未变。
- 崩溃率随运行方式/负载波动（12–45%），无输出即崩（main 早期、甚至第一条 require 前？——三份 gdb 样本全部崩在 T1-4000 循环中部，与"空输出"组合推断为 stdout 缓冲未刷新）。

## 4. 结论与处置（如实）

- **本测试的偶发 SEGFAULT 在本轮首次获得稳定 gdb 栈**：libstdc++ deque 反向迭代路径，位于本任务零改动的既有源单元内。无法归因到 B1 收口改动（改动不在其编译/链接单元，且源码未变）；但也**不能声明"无关"**——按任务要求标记为**既有环境/工具链偶发，根因未定位**。
- 复现命令（本机构建树）：
  `ctest -R "^physical_round_normalizer_test$"`（重复 8–40 次，约 1/8 ~ 1/2 概率 SEGFAULT）
  或直接 `build/tests_all_mingw_debug/paimage_core/physical_round_normalizer_test.exe`
- 建议后续（不属本任务范围）：以 `-fsanitize=address` 或 MSVC 构建复测该测试定位；或升级 MinGW 工具链验证是否为 13.1.0 deque 代码生成问题。
- 本轮完整 CTest 全量 47/47 通过（多次），单独重跑亦多次通过；失败概率与 45cd050 报告记录的偶发一致。**重复通过不证明根因已消失**——以上样本与栈为持续性证据。
