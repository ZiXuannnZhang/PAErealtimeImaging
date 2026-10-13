// norm_diag — 只读诊断（仓库外 scratch）：T1-4000 场景 + VEH 崩溃现场转储。
// 判定崩溃时 normalizer 对象（含 recentDecisions_ deque）是否被破坏。
#include "PaimageAcquisition/PhysicalRoundNormalizer.h"

#include <cstdio>
#include <cstring>
#include <windows.h>

using namespace paimage;

static PhysicalRoundNormalizer* g_norm = nullptr;
static bool g_inLoop = false;
static std::uint16_t g_lastTrigger = 0;

static void dumpObj(const char* when) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(g_norm);
    std::printf("  [%s] obj=%p bytes:\n", when, static_cast<const void*>(p));
    for (size_t row = 0; row < 24; ++row) {
        std::printf("    +%03zu:", row * 16);
        for (size_t i = 0; i < 16; ++i)
            std::printf(" %02x", p[row * 16 + i]);
        std::printf("\n");
    }
    std::fflush(stdout);
}

static LONG CALLBACK veh(PEXCEPTION_POINTERS ep) {
    if (!g_inLoop)
        return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != 0xC0000005)
        return EXCEPTION_CONTINUE_SEARCH;
    std::printf("CRASH: code=0x%08lX faultAddr=%p rip=%p rax=%p rbx=%p rcx=%p rdx=%p rsi=%p rdi=%p\n",
                static_cast<unsigned long>(code),
                ep->ExceptionRecord->ExceptionAddress,
                reinterpret_cast<void*>(ep->ContextRecord->Rip),
                reinterpret_cast<void*>(ep->ContextRecord->Rax),
                reinterpret_cast<void*>(ep->ContextRecord->Rbx),
                reinterpret_cast<void*>(ep->ContextRecord->Rcx),
                reinterpret_cast<void*>(ep->ContextRecord->Rdx),
                reinterpret_cast<void*>(ep->ContextRecord->Rsi),
                reinterpret_cast<void*>(ep->ContextRecord->Rdi));
    dumpObj("at-crash");
    std::fflush(stdout);
    ExitProcess(3);   // 已转储，直接终止（跳过栈展开）
}

int main() {
    AddVectoredExceptionHandler(1, veh);
    PhysicalRoundNormalizer normalizer(4000);
    g_norm = &normalizer;
    normalizer.beginSession(7000);
    normalizer.classify(7000, 0);
    g_inLoop = true;
    for (std::uint16_t trigger = 1; trigger <= 4000; ++trigger) {
        g_lastTrigger = trigger;
        auto r = normalizer.classify(7000, trigger);
        if (r.decision != PhysicalTriggerDecision::LogicalScan ||
            r.logicalTriggerIndex != trigger - 1) {
            std::printf("LOGIC-FAIL at trigger=%u\n", trigger);
            break;
        }
    }
    g_inLoop = false;
    auto s = normalizer.snapshot();
    std::printf("LOOP-COMPLETED cacheSize=%zu gen=%llu lastTrigger=%u\n",
                s.recentDecisionCacheSize,
                static_cast<unsigned long long>(s.roundGeneration),
                g_lastTrigger);
    return 0;
}
