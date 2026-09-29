判别性反证记录 — FrontendOnly 临时改回整组丢弃 → 转红 → 还原复绿
==================================================================

方法（任务文档验收 2 主选项）
----------------------------
临时改动 A（src/PaimageAcquisition/HostOutput.cpp，sync 内一处）：
把 FrontendOnly 判定从
    ...)frontendOnly=true;
改回
    ...)filter=true; // COUNTER-PROOF TEMP: revert FrontendOnly to whole-group drop
即恢复改动前的「超界组整组丢弃」行为（三态坍缩回单布尔 Drop）。

临时改动 B（tests/paimage_host_output_test.cpp，两处，如实披露）：
用 `if(false){ ... }` 守卫 G1/G2 块，使单二进制测试能执行到文件序靠后的
FO 块。原因：G1 修订断言（文件序最先的新契约断言）在反证状态下必先转红
并中止 main，FO3 将不可达。改动 B 仅影响反证运行，不改变断言本身；
随后与改动 A 一并精确还原。

临时改动 diff（还原后以 git diff 不可得，此处以文字+锚点记录两处 hunk；
改动 A/B 各自old/new 字符串均在本文件中逐字给出，可复核）

hunk A-old:
            if(normalizer_->disableCountBoundary() &&
               classification.decision==PhysicalTriggerDecision::LogicalScan &&
               classification.logicalTriggerIndex >= 0 &&
               static_cast<std::uint64_t>(classification.logicalTriggerIndex) >=
                   normalizer_->configuredLogicalTriggersPerRound())frontendOnly=true;
hunk A-new（反证态）:
            （同上，行尾 frontendOnly=true → filter=true; // COUNTER-PROOF TEMP: revert FrontendOnly to whole-group drop）

hunk B-old:
    {
        QTemporaryDir root(QDir::currentPath() + "/gate-XXXXXX");
hunk B-new（反证态）:
    {
        if(false){ // COUNTER-PROOF TEMP: G1/G2 guarded so the FO block is reached (restored after the counter-proof)
        QTemporaryDir root(QDir::currentPath() + "/gate-XXXXXX");
（以及块尾 output.stop(); 后插入 `} // COUNTER-PROOF TEMP guard end`。）

转红输出（counterproof_red.log，exit=3）
----------------------------------------
    PASS C1-C3/C8/C13 startup=7 disable=0 raw=4 realtime=4/card
    PASS C1-C3/C8/C13 startup=7 disable=1 raw=6 realtime=4/card
    terminate called after throwing an instance of 'std::runtime_error'
      what(): FO3 display keeps refreshing and save continues beyond the boundary
即任务文档点名的「displayUpdates 继续递增」断言（FO3）在整组丢弃回退态下
转红；C1-C3 两端迭代保持绿（新旧行为兼容性断言，符合设计）。

还原复绿（counterproof_green_restored.log，exit=0）
----------------------------------------------------
    PASS production source/output/host boundary: four cards, 28/70, ...
（G1/G2 与 FO1–FO6 全部恢复通过。）

还原后状态：两处临时改动逐字还原（经 Edit 工具精确回写），最终全量
CTest 51/51 于 aa9097e 重新执行（ctest_final_debug_51of51_aa9097e.log），
最终树上不存在任何 COUNTER-PROOF TEMP 残留。
