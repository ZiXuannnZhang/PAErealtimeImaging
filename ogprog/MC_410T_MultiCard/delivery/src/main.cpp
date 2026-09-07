#include <QApplication>
#include <QSettings>
#include <cstdio>
#include <csignal>
#include "MainWindow.h"
#include "Constants.h"

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>

// ── 全局日志句柄，所有 LOG() 调用写入此文件 ──────────────────────────
void* g_hLogFile = INVALID_HANDLE_VALUE;

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
}
#endif // _WIN32

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // ── 最优先：重定向日志到 app_log.txt ─────────────────
    redirectStdioToFile();
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
