#include <QApplication>
#include <QSettings>
#include "PaimageAcquisition/TraceBundle.h"
#include "PaimageAcquisition/BuildIdentity.h"
#include "PaimageAcquisition/SettingsPath.h"
#include <QJsonDocument>
#include <QDebug>
#include <QStringList>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSysInfo>
#include <QDateTime>
#include <QtConcurrent/QtConcurrentRun>
#include <QThreadPool>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <csignal>
#include <cstring>
#include "MainWindow.h"
#include "Constants.h"
#include "DiagnosticRecorder.h"
#include "StartupPolicy.h"

QStringList g_targetIPs;

namespace {
struct EarlyDiagnostic {
    QString message;
    DiagnosticRecorder::Severity severity;
    QJsonObject fields;
};
std::mutex earlyDiagnosticMutex;
QVector<EarlyDiagnostic> earlyDiagnostics;
std::atomic<bool> diagnosticReady{false};
void recordApplicationMessage(const QString &message, DiagnosticRecorder::Severity severity,
                              QJsonObject fields = {})
{
    static thread_local bool insideHandler = false;
    if (insideHandler) return;
    insideHandler = true;
    fields.insert(QStringLiteral("source"), QStringLiteral("application"));
    fields.insert(QStringLiteral("observedAt"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    if (diagnosticReady.load(std::memory_order_acquire)) {
        if (auto *recorder = DiagnosticRecorder::instance())
            recorder->recordEvent(QStringLiteral("application"), message, severity, fields);
    } else {
        std::lock_guard<std::mutex> lock(earlyDiagnosticMutex);
        if (earlyDiagnostics.size() < 256) earlyDiagnostics.append({message, severity, fields});
    }
    insideHandler = false;
}
}

static bool parseIPv4Literal(const QString& input, QString* canonical)
{
    const QStringList octets = input.trimmed().split('.', Qt::KeepEmptyParts);
    if (octets.size() != 4) return false;

    QStringList canonicalOctets;
    canonicalOctets.reserve(4);
    for (const QString& octet : octets) {
        if (octet.isEmpty() || octet.size() > 3) return false;
        for (const QChar ch : octet) {
            if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) return false;
        }

        bool ok = false;
        const int value = octet.toInt(&ok);
        if (!ok || value > 255) return false;
        canonicalOctets.append(QString::number(value));
    }

    *canonical = canonicalOctets.join('.');
    return true;
}

static bool parseTargetIPs(int argc, char* argv[], QStringList* parsed, QString* error)
{
    parsed->clear();
    for (int i = 1; i < argc; ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        QString rawValue;
        if (argument == QStringLiteral("--target-ips")) {
            if (i + 1 >= argc) {
                *error = QStringLiteral("--target-ips requires a comma-separated IPv4 list");
                return false;
            }
            rawValue = QString::fromLocal8Bit(argv[++i]);
        } else if (argument.startsWith(QStringLiteral("--target-ips="))) {
            rawValue = argument.mid(QStringLiteral("--target-ips=").size());
        } else {
            continue;
        }

        const QStringList values = rawValue.split(',', Qt::KeepEmptyParts);
        for (const QString& rawIP : values) {
            const QString input = rawIP.trimmed();
            if (input.isEmpty()) {
                *error = QStringLiteral("--target-ips contains an empty address");
                return false;
            }

            QString canonical;
            if (!parseIPv4Literal(input, &canonical)) {
                *error = QStringLiteral("--target-ips requires IPv4 literals: '%1'").arg(input);
                return false;
            }
            if (parsed->contains(canonical)) {
                *error = QStringLiteral("--target-ips contains duplicate address: '%1'")
                             .arg(canonical);
                return false;
            }
            parsed->append(canonical);
            if (parsed->size() > MAX_CARDS) {
                *error = QStringLiteral("--target-ips contains more than %1 addresses")
                             .arg(MAX_CARDS);
                return false;
            }
        }
    }
    return true;
}

static bool parseStartupArguments(int argc, char* argv[], StartupPolicy* policy,
                                  QString* trialId, QString* channelDir, QString* error)
{
    for (int i = 1; i < argc; ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        QString name, value;
        bool inlineValue = false;
        const int equals = argument.indexOf(QLatin1Char('='));
        if (argument.startsWith(QLatin1String("--")) && equals > 2) {
            name = argument.left(equals);
            value = argument.mid(equals + 1);
            inlineValue = true;
        }
        auto matches = [&](const char* option) {
            return argument == QLatin1String(option)
                || (inlineValue && name == QLatin1String(option));
        };
        auto nextValue = [&](const char* option) -> bool {
            if (inlineValue) return true;
            if (i + 1 >= argc) {
                *error = QStringLiteral("%1 requires a value").arg(QLatin1String(option));
                return false;
            }
            value = QString::fromLocal8Bit(argv[++i]);
            return true;
        };
        if (matches("--startup-policy")) {
            if (!nextValue("--startup-policy")) return false;
            StartupPolicy parsed = StartupPolicy::Bypass;
            if (!parseStartupPolicy(value.toStdString(), &parsed)) {
                *error = QStringLiteral("--startup-policy requires bypass or legacy, got '%1'").arg(value);
                return false;
            }
            *policy = parsed;
        } else if (matches("--startup-trial-id")) {
            if (!nextValue("--startup-trial-id")) return false;
            *trialId = value.trimmed();
        } else if (matches("--system-capture-channel")) {
            if (!nextValue("--system-capture-channel")) return false;
            *channelDir = value.trimmed();
        }
    }
    return true;
}

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
    auto severity = DiagnosticRecorder::Severity::Info;
    if (type == QtDebugMsg) severity = DiagnosticRecorder::Severity::Debug;
    if (type == QtWarningMsg) severity = DiagnosticRecorder::Severity::Warning;
    if (type == QtCriticalMsg) severity = DiagnosticRecorder::Severity::Error;
    if (type == QtFatalMsg) severity = DiagnosticRecorder::Severity::Critical;
    recordApplicationMessage(msg, severity, {{"qtCategory", QString::fromUtf8(ctx.category ? ctx.category : "")},
        {"file", QString::fromUtf8(ctx.file ? ctx.file : "")}, {"line", ctx.line}});
    const char *level = "INFO";
    switch (type) {
    case QtDebugMsg:    level = "DEBUG"; break;
    case QtInfoMsg:     level = "INFO";  break;
    case QtWarningMsg:  level = "WARN";  break;
    case QtCriticalMsg: level = "CRIT";  break;
    case QtFatalMsg:    level = "FATAL"; break;
    }

    const QByteArray local = msg.toUtf8();
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
    n = qMin(n, static_cast<int>(sizeof(buf) - 3));
    recordApplicationMessage(QString::fromUtf8(buf, n).trimmed(), DiagnosticRecorder::Severity::Info);

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
    QString targetIPsError;
    if (!parseTargetIPs(argc, argv, &g_targetIPs, &targetIPsError)) {
        const QByteArray errorBytes = targetIPsError.toLocal8Bit();
        std::fprintf(stderr, "error: %s\n", errorBytes.constData());
        return 2;
    }
    StartupPolicy startupPolicyValue = StartupPolicy::Bypass;
    QString startupTrialIdValue, startupChannelValue, startupArgumentsError;
    if (!parseStartupArguments(argc, argv, &startupPolicyValue, &startupTrialIdValue,
                               &startupChannelValue, &startupArgumentsError)) {
        const QByteArray errorBytes = startupArgumentsError.toLocal8Bit();
        std::fprintf(stderr, "error: %s\n", errorBytes.constData());
        return 2;
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
    app.setApplicationName("PAimage接收诊断版");
    app.setApplicationVersion("1.1.0-receiver-diagnostics");
    app.setOrganizationName("PAimageReceiverDiagnostics");
    const std::string captureChannelPath = startupChannelValue.isEmpty()
        ? (QCoreApplication::applicationDirPath() + "/system-capture-channel").toStdString()
        : startupChannelValue.toStdString();
    configureStartupDiagnostics(startupPolicyValue,startupTrialIdValue.toStdString(),captureChannelPath);

    QString diagnosticError;
    const bool copiedLegacyParameters=seedPaimageSettings();
    paimage::installTraceBundle(QCoreApplication::applicationDirPath()+"/paimage-traces",QCoreApplication::applicationDirPath()+"/diagnostic-tools");
    DiagnosticRecorder::Options diagnosticOptions;
    diagnosticOptions.rootDirectory=QCoreApplication::applicationDirPath()+"/paimage-diagnostics";
    auto *recorder = DiagnosticRecorder::initialize(diagnosticOptions,&diagnosticError);
    if (recorder) {
        std::lock_guard<std::mutex> lock(earlyDiagnosticMutex);
        diagnosticReady.store(true, std::memory_order_release);
        for (const auto &entry : earlyDiagnostics)
            recorder->recordEvent("application", entry.message, entry.severity, entry.fields);
        earlyDiagnostics.clear();
    }
    const QString executablePath = QCoreApplication::applicationFilePath();
    QJsonObject identity{{"kind", "program"}, {"executablePath", executablePath},
        {"backendId","paimage-receiver-diagnostics"},
        {"behaviorMappingVersion","receiver-diagnostics-1"},
        {"parameterStore",paimageSettingsPath()},{"copiedLegacyParameters",copiedLegacyParameters},
        {"sourceGitSha",PAIMAGE_GIT_SHA},{"trackedSourceDirty",PAIMAGE_TRACKED_DIRTY},
        {"buildType",PAIMAGE_BUILD_TYPE},{"compiler",PAIMAGE_COMPILER},
        {"productBaseline",PAIMAGE_PRODUCT_BASELINE},{"sourcePaimageSha256",PAIMAGE_SOURCE_SHA256},
        {"applicationVersion", app.applicationVersion()}, {"qtVersion", qVersion()},
        {"build", QStringLiteral(__DATE__ " " __TIME__)},
        {"os", QSysInfo::prettyProductName()}, {"architecture", QSysInfo::currentCpuArchitecture()},
        {"processId", static_cast<double>(QCoreApplication::applicationPid())},
        {"arguments", QJsonArray::fromStringList(QCoreApplication::arguments())},
        {"startupPolicy", startupPolicyName(startupPolicy())},
        {"startupTrialId", QString::fromStdString(startupTrialId())},
        {"systemCaptureChannelDir", QString::fromStdString(systemCaptureChannelDir())},
        {"targetIPs", QJsonArray::fromStringList(g_targetIPs)}, {"workingDirectory", QDir::currentPath()}};
    if (recorder) recorder->recordSettingsSnapshot(identity);
    auto identityJob = QtConcurrent::run([executablePath, identity]() mutable {
        QFile executable(executablePath);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (executable.open(QIODevice::ReadOnly) && hash.addData(&executable))
            identity.insert("executableSha256", QString::fromLatin1(hash.result().toHex()));
        else identity.insert("identityError", executable.errorString());
        QJsonArray libraries;
        const QDir directory(QFileInfo(executablePath).absolutePath());
        for(const auto& name:{QString("Qt6Core.dll"),QString("ring_recon_cuda.dll"),QString("cufft64_12.dll"),QString("ImagingSvc.exe"),QString("libzmq-v141-mt-4_3_5.dll")}){
            QFile file(directory.filePath(name));QCryptographicHash digest(QCryptographicHash::Sha256);
            QJsonObject entry{{"file",name}};
            if(file.open(QIODevice::ReadOnly)&&digest.addData(&file))entry.insert("sha256",QString::fromLatin1(digest.result().toHex()));
            else entry.insert("status","unknown: unavailable or unreadable");
            libraries.append(entry);
        }
        identity.insert("keyBinaryHashes",libraries);
        QFile manifest(directory.filePath("build-manifest.json"));
        if(manifest.open(QIODevice::ReadOnly))identity.insert("buildManifest",QJsonDocument::fromJson(manifest.readAll()).object());
        if (auto *r = DiagnosticRecorder::instance()) r->recordSettingsSnapshot(identity);
    });

    int ret = 0;
    {
        MainWindow window;
        window.setWindowTitle(QStringLiteral("PAimage接收诊断版 — receiver-diagnostics — ")+QString::fromLatin1(PAIMAGE_GIT_SHA).left(12));
        window.show();
        if (!diagnosticError.isEmpty()) qWarning().noquote() << "诊断日志初始化：" << diagnosticError;
        ret = app.exec();
    }
    identityJob.waitForFinished();
    // Export/background inspection jobs must finish before destroying the recorder.
    QThreadPool::globalInstance()->waitForDone();
    if (recorder) recorder->recordEvent("application", "程序正常退出", DiagnosticRecorder::Severity::Info,
                                      {{"exitCode", ret}});
    diagnosticReady.store(false, std::memory_order_release);
    DiagnosticRecorder::shutdown();

    return ret;
}
