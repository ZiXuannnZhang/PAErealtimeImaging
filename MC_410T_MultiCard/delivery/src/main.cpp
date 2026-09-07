#include <QApplication>
#include <QSettings>
#include <QDebug>
#include <cstdio>
#include <csignal>
#include <cstring>
#include "MainWindow.h"
#include "Constants.h"

QStringList g_targetIPs;

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <fcntl.h>
#include <io.h>

// ── 全局日志句柄，所有 LOG() 调用写入此文件 ──────────────────────────
void* g_hLogFile = INVALID_HANDLE_VALUE;

// ── 崩溃转储（首异常向量处理器）：任何线程的严重异常都先写一份 minidump ──
typedef BOOL(WINAPI *MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, DWORD,
                                          MINIDUMP_EXCEPTION_INFORMATION*, void*, void*);

static void writeCrashDump(EXCEPTION_POINTERS *ep)
{
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    char *lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    char dumpDir[MAX_PATH] = {};
    snprintf(dumpDir, MAX_PATH, "%scrashdumps", exePath);
    CreateDirectoryA(dumpDir, nullptr);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char dumpPath[MAX_PATH] = {};
    snprintf(dumpPath, MAX_PATH, "%s\\MC410T_Receiver_%04d%02d%02d_%02d%02d%02d.dmp",
             dumpDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    HANDLE hFile = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    if (dbghelp) {
        MiniDumpWriteDumpFn fn = reinterpret_cast<MiniDumpWriteDumpFn>(
            GetProcAddress(dbghelp, "MiniDumpWriteDump"));
        if (fn) {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers = FALSE;
            // 0 = MiniDumpNormal（含线程栈与模块信息，足以分析崩溃栈；
            //     全内存转储在堆已损坏时容易失败）
            const BOOL ok = fn(GetCurrentProcess(), GetCurrentProcessId(), hFile, 0,
                               &mei, nullptr, nullptr);
            char buf[256];
            int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "[CRASH] MiniDumpWriteDump ok=%d err=%lu path=%s\r\n",
                ok ? 1 : 0, GetLastError(), dumpPath);
            DWORD w = 0;
            if (g_hLogFile != INVALID_HANDLE_VALUE && n > 0) {
                WriteFile((HANDLE)g_hLogFile, buf, (DWORD)n, &w, nullptr);
                FlushFileBuffers((HANDLE)g_hLogFile);
            }
        } else {
            const char *msg = "[CRASH] GetProcAddress(MiniDumpWriteDump) failed\r\n";
            DWORD w = 0;
            if (g_hLogFile != INVALID_HANDLE_VALUE)
                WriteFile((HANDLE)g_hLogFile, msg, (DWORD)strlen(msg), &w, nullptr);
        }
    } else {
        const char *msg = "[CRASH] LoadLibrary(dbghelp.dll) failed\r\n";
        DWORD w = 0;
        if (g_hLogFile != INVALID_HANDLE_VALUE)
            WriteFile((HANDLE)g_hLogFile, msg, (DWORD)strlen(msg), &w, nullptr);
    }
    CloseHandle(hFile);
}

// 把崩溃线程的调用栈写成文本（StackWalk64），便于无调试器时直接定位；
// 与 minidump 一起写入 bin/crashdumps。
static void writeCrashBacktrace(EXCEPTION_POINTERS *ep)
{
    if (!ep || !ep->ContextRecord) return;

    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    char *lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    char dumpDir[MAX_PATH] = {};
    snprintf(dumpDir, MAX_PATH, "%scrashdumps", exePath);
    CreateDirectoryA(dumpDir, nullptr);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char btPath[MAX_PATH] = {};
    snprintf(btPath, MAX_PATH, "%s\\crash_bt_%04d%02d%02d_%02d%02d%02d.txt",
             dumpDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    HANDLE hFile = CreateFileA(btPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    auto logLine = [&](const char *s) {
        DWORD w = 0;
        WriteFile(hFile, s, (DWORD)strlen(s), &w, nullptr);
    };

    char hdr[256];
    int n = _snprintf_s(hdr, sizeof(hdr), _TRUNCATE,
        "code=0x%08lX addr=%p thread=%lu\n",
        ep->ExceptionRecord->ExceptionCode,
        ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
    if (n > 0) logLine(hdr);

#if defined(_M_X64)
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf{};
    sf.AddrPC.Offset    = ctx.Rip;
    sf.AddrPC.Mode      = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrFrame.Mode   = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp;
    sf.AddrStack.Mode   = AddrModeFlat;

    for (int i = 0; i < 64; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(),
                         &sf, &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr))
            break;
        if (sf.AddrPC.Offset == 0) break;
        char mod[MAX_PATH] = "?";
        HMODULE hm = reinterpret_cast<HMODULE>(
            SymGetModuleBase64(GetCurrentProcess(), sf.AddrPC.Offset));
        if (hm) GetModuleFileNameA(hm, mod, MAX_PATH);
        char line[512];
        int m = _snprintf_s(line, sizeof(line), _TRUNCATE,
            "  %02d 0x%llX %s\n", i,
            static_cast<unsigned long long>(sf.AddrPC.Offset), mod);
        if (m > 0) logLine(line);
    }
#endif

    CloseHandle(hFile);
}

static LONG CALLBACK crashVectoredHandler(PEXCEPTION_POINTERS ep)
{
    const DWORD code = ep ? ep->ExceptionRecord->ExceptionCode : 0;
    // 只处理严重异常；调试断点/C++ 异常等常规码不打扰
    switch (code) {
    case 0xC0000005:  // 访问违例
    case 0xC0000602:  // fail-fast
    case 0xC0000374:  // 堆损坏
    case 0xC00000FD:  // 栈溢出
    case 0xC0000409:  // 栈缓冲区溢出/快速失败
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }

    char buf[256];
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[CRASH] VectoredHandler code=0x%08lX addr=%p thread=%lu\r\n",
        code, ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
    DWORD w = 0;
    if (g_hLogFile != INVALID_HANDLE_VALUE && n > 0) {
        WriteFile((HANDLE)g_hLogFile, buf, (DWORD)n, &w, nullptr);
        FlushFileBuffers((HANDLE)g_hLogFile);
    }
    OutputDebugStringA(buf);

    writeCrashBacktrace(ep);
    writeCrashDump(ep);
    return EXCEPTION_CONTINUE_SEARCH;   // 继续交给系统/WER
}

// ── Qt 消息处理器：把 qDebug/qWarning/qCritical/qFatal 全部写入 app_log，
//    崩溃前 Qt 的致命信息（如“Out of memory”、Q_ASSERT 文本）不再丢失 ──
static void qtMessageHandler(QtMsgType type, const QMessageLogContext &ctx,
                             const QString &msg)
{
    const char *level = "INFO";
    switch (type) {
    case QtDebugMsg:    level = "DEBUG"; break;
    case QtInfoMsg:     level = "INFO";  break;
    case QtWarningMsg:  level = "WARN";  break;
    case QtCriticalMsg: level = "CRIT";  break;
    case QtFatalMsg:    level = "FATAL"; break;
    }

    const QByteArray local = msg.toLocal8Bit();
    const char *file = ctx.file ? ctx.file : "?";
    char buf[2048];
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[QT:%s] %s (%s:%u)\r\n", level, local.constData(), file, ctx.line);
    if (n <= 0) return;
    if (g_hLogFile != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile((HANDLE)g_hLogFile, buf, (DWORD)n, &w, nullptr);
        FlushFileBuffers((HANDLE)g_hLogFile);
    }
    OutputDebugStringA(buf);
}

// 直接用 WriteFile 写日志（完全绕过 CRT stdout/stderr，GUI 进程可靠）
static void LOG(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n <= 0) return;

    // OutputDebugStringA：VS Code 调试输出 / DebugView 可见（无需控制台）
    OutputDebugStringA(buf);

    if (g_hLogFile == INVALID_HANDLE_VALUE) return;
    char out[4096];
    int m = 0;
    for (int i = 0; i < n && m < (int)sizeof(out) - 2; ++i) {
        if (buf[i] == '\n' && (i == 0 || buf[i-1] != '\r'))
            out[m++] = '\r';
        out[m++] = buf[i];
    }
    DWORD w = 0;
    WriteFile((HANDLE)g_hLogFile, out, (DWORD)m, &w, nullptr);
    FlushFileBuffers((HANDLE)g_hLogFile);
}

static void redirectStdioToFile() {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    char logPath[MAX_PATH] = {};
    snprintf(logPath, MAX_PATH, "%sapp_log.txt", exePath);

    HANDLE hFile = CreateFileA(logPath,
        GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    // 新文件写 UTF-8 BOM
    LARGE_INTEGER fileSize = {};
    GetFileSizeEx(hFile, &fileSize);
    if (fileSize.QuadPart == 0) {
        DWORD w = 0;
        const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
        WriteFile(hFile, bom, sizeof(bom), &w, nullptr);
    }
    SetFilePointer(hFile, 0, nullptr, FILE_END);
    g_hLogFile = (void*)hFile;

    SYSTEMTIME st; GetLocalTime(&st);
    LOG("\n=== SESSION %04d-%02d-%02d %02d:%02d:%02d ===\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    LOG("[LOG] redirectStdioToFile OK\n");

    // ── 安装崩溃捕获 ─────────────────────────────────────
    // 1. SIGABRT: 捕获 CRT abort()
    std::signal(SIGABRT, [](int) {
        const char* msg = "[FATAL] SIGABRT: abort() called\r\n";
        DWORD w = 0;
        if (g_hLogFile != INVALID_HANDLE_VALUE)
            WriteFile((HANDLE)g_hLogFile, msg, (DWORD)strlen(msg), &w, nullptr);
        OutputDebugStringA(msg);
        std::signal(SIGABRT, SIG_DFL);
        std::raise(SIGABRT);
    });

    // 2. SetUnhandledExceptionFilter: 捕获所有未处理 SEH
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
        char buf[256];
        int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[FATAL] UnhandledExceptionFilter code=0x%08lX addr=%p\r\n",
            ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
        DWORD w = 0;
        if (g_hLogFile != INVALID_HANDLE_VALUE && n > 0)
            WriteFile((HANDLE)g_hLogFile, buf, (DWORD)n, &w, nullptr);
        OutputDebugStringA(buf);
        FlushFileBuffers((HANDLE)g_hLogFile);
        return EXCEPTION_CONTINUE_SEARCH;  // 让系统继续处理（生成 WER 报告）
    });

    // 3. 首异常向量处理器：部分崩溃（fail-fast/被 Qt 捕获的异常）不会经过
    //    SetUnhandledExceptionFilter，这里在任何严重异常首次派发时先写 minidump。
    AddVectoredExceptionHandler(1, crashVectoredHandler);
}
#endif // _WIN32

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        // 固定目标采集卡 IP（逗号分隔，如 --target-ips 127.0.0.1,127.0.0.1）。
        // 真机固定 IP 与同机 UDP 联调均可用；省略时自动执行网段扫描。
        if (std::strcmp(argv[i], "--target-ips") == 0 && i + 1 < argc) {
            const QStringList ips = QString::fromLocal8Bit(argv[i + 1])
                                        .split(',', Qt::SkipEmptyParts);
            g_targetIPs = ips;
            ++i;
        }
    }
    // 单个目标 IP 自动扩展为 4 张卡（同机联调/同网段多卡共用地址）
    if (g_targetIPs.size() == 1) {
        const QString ip = g_targetIPs.front();
        g_targetIPs.clear();
        for (int c = 0; c < 4; ++c) g_targetIPs << ip;
    }
#ifdef _WIN32
    // ── 最优先：重定向日志到 app_log.txt ─────────────────
    redirectStdioToFile();
    qInstallMessageHandler(qtMessageHandler);
#endif

    // ── Qt 应用初始化 ────────────────────────────────────────────────
    QApplication app(argc, argv);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableAccessibility);   // Qt5: 启用 UIA 支持
#endif
    app.setApplicationName("MC410T 多卡接收处理系统");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("MC_410T");

    MainWindow window;
    window.show();

    int ret = app.exec();

    return ret;
}
