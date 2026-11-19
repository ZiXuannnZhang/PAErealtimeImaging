#include "RingPngWriter.h"
#include "RingNativeMapping.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QThreadPool>
#include <QUuid>
#include <QWinEventNotifier>
#include <algorithm>
#include <cstring>
#include <deque>
#include <io.h>
#include <new>
#include <string>
#include <utility>

namespace ring_png {
namespace {
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kStride = kEdge * sizeof(QRgb);
constexpr std::size_t kPathUnits = 32768;

enum class Receipt : std::uint32_t {
    None, Saved, DirectoryFailed, Collision, FirstFailed, SecondFailed, Partial, Exception
};

// The request and reply events hand off exclusive slot ownership. The host
// publishes task; the writer publishes receiptTask only after both commits.
struct SharedSlot {
    std::uint32_t version, stride, bytes;
    DWORD parentPid;
    std::uint64_t parentStarted, task, receiptTask;
    Receipt receipt;
    wchar_t paths[2][kPathUnits];
    alignas(QRgb) unsigned char pixels[kPairBytes];
};

struct CloseHandleOnExit {
    void operator()(HANDLE handle) const { if (handle) CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandleOnExit>;

struct Transport {
    ring_input::NativeMapping mapping;
    Handle request, reply, stop, process;
#ifdef MAINWINDOW_START_TEST_SEAM
    Handle gate, blocked;
    DWORD processId = 0;
#endif

    SharedSlot* slot() const {
        return mapping.capacity() >= sizeof(SharedSlot) ? static_cast<SharedSlot*>(mapping.data()) : nullptr;
    }
};

std::wstring name(const std::wstring& instance, const wchar_t* field)
{
    return L"Local\\PAimage_RingPng_" + instance + field;
}

Handle newEvent(const std::wstring& instance, const wchar_t* field, bool manualReset = false)
{
    Handle event(CreateEventW(nullptr, manualReset, FALSE, name(instance, field).c_str()));
    if (GetLastError() == ERROR_ALREADY_EXISTS) return {};
    return event;
}

Handle openEvent(const std::wstring& instance, const wchar_t* field, DWORD access)
{
    return Handle(OpenEventW(access, FALSE, name(instance, field).c_str()));
}

bool signaled(HANDLE handle)
{
    return handle && WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
}

std::uint64_t startedAt(HANDLE process)
{
    FILETIME started{}, ended{}, kernel{}, user{};
    if (!GetProcessTimes(process, &started, &ended, &kernel, &user)) return 0;
    return (std::uint64_t(started.dwHighDateTime) << 32) | started.dwLowDateTime;
}

Result resultFor(Receipt receipt, const Paths& paths)
{
    const char* status = "writer_protocol_error";
    switch (receipt) {
    case Receipt::Saved: status = "saved"; break;
    case Receipt::DirectoryFailed: status = "directory_failed"; break;
    case Receipt::Collision: status = "path_collision"; break;
    case Receipt::FirstFailed: status = "first_png_failed"; break;
    case Receipt::SecondFailed: status = "second_png_failed"; break;
    case Receipt::Partial: status = "partial_png"; break;
    case Receipt::Exception: status = "write_exception"; break;
    case Receipt::None: break;
    }
    return {QString::fromLatin1(status), receipt == Receipt::Partial ? paths[0] : QString(), paths};
}

std::wstring nativePath(const QString& path)
{
    auto native = QDir::toNativeSeparators(path).toStdWString();
    if (native.compare(0, 4, L"\\\\?\\") == 0) return native;
    if (native.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + native.substr(2);
    return L"\\\\?\\" + native;
}

bool publishFile(const QString& temporary, const QString& target)
{
    // The temporary lives beside its target. Publish atomically, including
    // when another writer creates the target after the initial check.
    return MoveFileW(nativePath(temporary).c_str(), nativePath(target).c_str());
}

// Match the best-effort sync previously done by QSaveFile::commit().
void syncTempFile(QTemporaryFile& file)
{
    const int descriptor = file.handle();
    if (descriptor < 0) return;
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
    if (handle != INVALID_HANDLE_VALUE) FlushFileBuffers(handle);
}

bool encodeTemp(const QImage& image, const QString& path, QString& written,
                const std::function<void()>& beforeSync = {})
{
    QTemporaryFile file(path + QStringLiteral(".XXXXXX"));
    if (!file.open() || !image.save(&file, "PNG") || !file.flush()) return false;
    if (beforeSync) beforeSync();
    syncTempFile(file);
    written = file.fileName();
    file.setAutoRemove(false);
    return true;
}

Receipt commit(const Pair& pair, const Paths& paths, const std::function<void()>& afterFirst = {},
               const std::function<void()>& beforeSyncFirst = {})
{
    if (!QDir().mkpath(QFileInfo(paths[0]).absolutePath())) return Receipt::DirectoryFailed;
    if (paths[0] == paths[1] || QFileInfo::exists(paths[0]) || QFileInfo::exists(paths[1]))
        return Receipt::Collision;

    struct RemoveTemps {
        Paths files;
        ~RemoveTemps() { for (const auto& file : files) if (!file.isEmpty()) QFile::remove(file); }
    } temps;
    if (!encodeTemp(pair[0], paths[0], temps.files[0], beforeSyncFirst)) return Receipt::FirstFailed;
    if (!encodeTemp(pair[1], paths[1], temps.files[1])) return Receipt::SecondFailed;
    if (!publishFile(temps.files[0], paths[0])) return Receipt::FirstFailed;
    temps.files[0].clear();
    if (afterFirst) afterFirst();
    if (publishFile(temps.files[1], paths[1])) {
        temps.files[1].clear();
        return Receipt::Saved;
    }
    return QFile::remove(paths[0]) ? Receipt::SecondFailed : Receipt::Partial;
}

bool validPixels(const Pair& images)
{
    return std::all_of(images.begin(), images.end(), [](const QImage& image) {
        return image.size() == QSize(kEdge, kEdge) && image.format() == QImage::Format_ARGB32 &&
               image.bytesPerLine() == kStride && image.sizeInBytes() == kPairBytes / 2;
    });
}

Paths decodePaths(const SharedSlot& slot)
{
    Paths paths;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const auto* start = slot.paths[i];
        const auto* end = std::find(start, start + kPathUnits, L'\0');
        if (end == start || end == start + kPathUnits) return {};
        paths[i] = QString::fromWCharArray(start, end - start);
        if (!QDir::isAbsolutePath(paths[i])) return {};
    }
    return paths;
}

bool encodePaths(SharedSlot& slot, const Paths& paths)
{
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (!QDir::isAbsolutePath(paths[i]) || paths[i].size() >= qsizetype(kPathUnits) ||
            paths[i].contains(QChar(u'\0'))) return false;
        const auto count = paths[i].toWCharArray(slot.paths[i]);
        slot.paths[i][count] = L'\0';
    }
    return true;
}
} // namespace

Paths targetPaths(const QString& dir, const QString& suffix, const QString& tag)
{
    if (dir.isEmpty()) return {};
    const auto time = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    const auto trimmed = suffix.trimmed();
    const auto base = (trimmed.isEmpty() ? time : trimmed + QStringLiteral("_") + time) + tag;
    const QDir target(dir);
    return {QDir::cleanPath(target.absoluteFilePath(base + QStringLiteral("_532nm.png"))),
            QDir::cleanPath(target.absoluteFilePath(base + QStringLiteral("_1064nm.png")))};
}

Result commitPair(const Pair& pair, const Paths& paths)
{
    if (pair[0].isNull() || pair[1].isNull()) return {QStringLiteral("render_failed"), {}};
    if (paths[0].isEmpty() || paths[1].isEmpty()) return {QStringLiteral("directory_failed"), {}};
    const Paths absolute{QFileInfo(paths[0]).absoluteFilePath(), QFileInfo(paths[1]).absoluteFilePath()};
    return resultFor(commit(pair, absolute), absolute);
}

std::optional<int> runWriterMode(int argc, char** argv)
{
    if (argc < 2 || std::strcmp(argv[1], "--ring-png-writer") != 0) return {};
#ifdef MAINWINDOW_START_TEST_SEAM
    if (argc != 3 && argc != 4) return 2;
    const std::string testMode = argc == 4 ? argv[3] : "";
#else
    if (argc != 3) return 2;
#endif
    QCoreApplication app(argc, argv);
    const auto token = QString::fromLatin1(argv[2]);
    const QUuid id(QStringLiteral("{") + token + QStringLiteral("}"));
    if (id.isNull() || id.toString(QUuid::WithoutBraces) != token) return 2;
    const auto instance = token.toStdWString();
    Transport io;
    if (io.mapping.attach(name(instance, L"_map").c_str()) != ring_input::Result::Ok) return 3;
    auto* slot = io.slot();
    if (!slot || slot->version != kVersion) return 3;
    Handle parent(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, slot->parentPid));
    const auto parentStarted = parent ? startedAt(parent.get()) : 0;
    if (!parentStarted || parentStarted != slot->parentStarted) return 3;
    io.request = openEvent(instance, L"_request", SYNCHRONIZE);
    io.reply = openEvent(instance, L"_reply", EVENT_MODIFY_STATE);
    io.stop = openEvent(instance, L"_stop", SYNCHRONIZE);
    if (!io.request || !io.reply || !io.stop) return 3;
    const HANDLE waits[]{parent.get(), io.stop.get(), io.request.get()};
#ifdef MAINWINDOW_START_TEST_SEAM
    if (testMode == "block") {
        io.gate = openEvent(instance, L"_testGate", SYNCHRONIZE);
        io.blocked = openEvent(instance, L"_testBlocked", EVENT_MODIFY_STATE);
        if (!io.gate || !io.blocked) return 3;
    }
#endif

    std::uint64_t previous = 0;
    for (;;) {
        const DWORD wait = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_OBJECT_0 + 1) return 0;
        if (wait != WAIT_OBJECT_0 + 2 || slot->version != kVersion || slot->stride != kStride ||
            slot->bytes != kPairBytes || !slot->task || slot->task == previous) return 3;
        previous = slot->task;
        const auto paths = decodePaths(*slot);
        if (paths[0].isEmpty() || paths[1].isEmpty()) return 3;
        const Pair images{
            QImage(static_cast<const uchar*>(slot->pixels), kEdge, kEdge, kStride, QImage::Format_ARGB32),
            QImage(static_cast<const uchar*>(slot->pixels) + kPairBytes / 2, kEdge, kEdge, kStride, QImage::Format_ARGB32)
        };
        Receipt receipt = Receipt::Exception;
        try {
#ifdef MAINWINDOW_START_TEST_SEAM
            if (io.gate) {
                receipt = commit(images, paths, {}, [&] {
                    // First temporary PNG has been encoded and written.
                    if (!SetEvent(io.blocked.get())) ExitProcess(3);
                    const HANDLE testWaits[]{io.gate.get(), parent.get(), io.stop.get()};
                    if (WaitForMultipleObjects(3, testWaits, FALSE, INFINITE) != WAIT_OBJECT_0) ExitProcess(0);
                });
            } else if (testMode == "exit-after-first") {
                receipt = commit(images, paths, [] { ExitProcess(7); });
            } else
#endif
            {
                receipt = commit(images, paths);
            }
        } catch (...) {}
        slot->receipt = receipt;
        slot->receiptTask = previous;
        if (!SetEvent(io.reply.get())) return 3;
    }
}

struct Channel::State {
    struct Job {
        std::uint64_t id;
        Pair images;
        Paths paths;
    };

    Channel& owner;
    Completion complete;
    QThreadPool staging;
    std::deque<Job> pending;
    std::optional<Job> current;
    std::shared_ptr<Transport> io;
    std::unique_ptr<QWinEventNotifier> receiptEvent, exitEvent;
    bool sent = false;
    bool closed = false;
#ifdef MAINWINDOW_START_TEST_SEAM
    TestMode testMode = TestMode::Normal;
    QString testExecutable;
#endif

    State(Channel& owner, Completion complete) : owner(owner), complete(std::move(complete)) {
        staging.setMaxThreadCount(1);
    }

    bool start()
    {
        const auto instance = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdWString();
        auto child = std::make_shared<Transport>();
        if (child->mapping.create(sizeof(SharedSlot), name(instance, L"_map").c_str()) != ring_input::Result::Ok)
            return false;
        auto* slot = new (child->mapping.data()) SharedSlot;
        slot->version = kVersion;
        slot->parentPid = GetCurrentProcessId();
        slot->parentStarted = startedAt(GetCurrentProcess());
        child->request = newEvent(instance, L"_request");
        child->reply = newEvent(instance, L"_reply", true);
        child->stop = newEvent(instance, L"_stop", true);
        if (!slot->parentStarted || !child->request || !child->reply || !child->stop) return false;

        auto executable = QCoreApplication::applicationFilePath();
#ifdef MAINWINDOW_START_TEST_SEAM
        if (!testExecutable.isEmpty()) executable = testExecutable;
#endif
        const auto exe = QDir::toNativeSeparators(executable).toStdWString();
        auto command = L"\"" + exe + L"\" --ring-png-writer " + instance;
#ifdef MAINWINDOW_START_TEST_SEAM
        if (testMode == TestMode::FailStart) return false;
        if (testMode == TestMode::ExitAfterFirst) command += L" exit-after-first";
        if (testMode == TestMode::Block) {
            command += L" block";
            child->gate = newEvent(instance, L"_testGate", true);
            child->blocked = newEvent(instance, L"_testBlocked", true);
            if (!child->gate || !child->blocked) return false;
        }
#endif
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return false;
        child->process.reset(process.hProcess);
        CloseHandle(process.hThread);
#ifdef MAINWINDOW_START_TEST_SEAM
        child->processId = process.dwProcessId;
#endif
        io = std::move(child);
        receiptEvent = std::make_unique<QWinEventNotifier>(io->reply.get());
        exitEvent = std::make_unique<QWinEventNotifier>(io->process.get());
        auto notify = [this] { collect(); pump(); };
        QObject::connect(receiptEvent.get(), &QWinEventNotifier::activated, &owner, notify);
        QObject::connect(exitEvent.get(), &QWinEventNotifier::activated, &owner, notify);
        return true;
    }

    void finish(Result result)
    {
        auto job = std::exchange(current, std::nullopt);
        sent = false;
        if (job) {
            result.paths = std::move(job->paths);
            complete(job->id, std::move(result));
        }
    }

    void disconnectTransport()
    {
        receiptEvent.reset();
        exitEvent.reset();
        io.reset(); // An in-progress local copy owns its distinct mapping view.
    }

    void collect()
    {
        if (!io) return;
        const bool exited = signaled(io->process.get());
        // A manual-reset reply stays observable at the shutdown deadline,
        // even when its Qt event has yet to run.
        if (signaled(io->reply.get())) {
            ResetEvent(io->reply.get());
            const auto* slot = io->slot();
            if (sent && current && slot->receiptTask == current->id)
                finish(resultFor(slot->receipt, current->paths));
        }
        if (exited) {
            disconnectTransport();
            if (sent) finish({QStringLiteral("writer_exited_unconfirmed"), {}});
        }
    }

    void pump()
    {
        while (!closed && !current && !pending.empty()) {
            current = std::move(pending.front());
            pending.pop_front();
            if (!io && !start()) {
                finish({QStringLiteral("writer_start_failed"), {}});
                continue;
            }
            const auto transport = io;
            if (!encodePaths(*transport->slot(), current->paths)) {
                finish({QStringLiteral("writer_invalid_target"), {}});
                continue;
            }
            const auto id = current->id;
            staging.start([this, transport, id, images = std::move(current->images)] {
                auto* slot = transport->slot();
                for (std::size_t i = 0; i < images.size(); ++i)
                    std::memcpy(slot->pixels + i * kPairBytes / 2, images[i].constBits(), kPairBytes / 2);
                QMetaObject::invokeMethod(&owner, [this, transport, id] {
                    if (closed || !current || current->id != id) return;
                    if (io != transport || signaled(io->process.get())) {
                        if (io == transport) disconnectTransport();
                        finish({QStringLiteral("writer_start_failed"), {}});
                        pump();
                        return;
                    }
                    auto* slot = transport->slot();
                    slot->stride = kStride;
                    slot->bytes = kPairBytes;
                    slot->task = id;
                    sent = SetEvent(transport->request.get());
                    if (!sent) {
                        finish({QStringLiteral("writer_signal_failed"), {}});
                        pump();
                    }
                }, Qt::QueuedConnection);
            });
        }
    }

    void shutdown()
    {
        if (std::exchange(closed, true)) return;
        collect(); // Take an already committed receipt before declaring expiry.
        if (io) {
            if (sent) TerminateProcess(io->process.get(), 1);
            else SetEvent(io->stop.get());
            disconnectTransport();
        }
        if (current) finish({sent ? QStringLiteral("shutdown_unconfirmed") :
                                     QStringLiteral("application_closed"), {}});
        while (!pending.empty()) {
            auto job = std::move(pending.front());
            pending.pop_front();
            complete(job.id, {QStringLiteral("application_closed"), {}, std::move(job.paths)});
        }
        staging.clear();
    }
};

Channel::Channel(Completion completion) : state_(std::make_unique<State>(*this, std::move(completion))) {}
Channel::~Channel() { shutdown(); state_->staging.waitForDone(); }

void Channel::enqueue(std::uint64_t id, Pair images, Paths paths)
{
    if (state_->closed || !validPixels(images)) {
        state_->complete(id, {state_->closed ? QStringLiteral("application_closed") :
                                              QStringLiteral("render_failed"), {}, std::move(paths)});
        return;
    }
    state_->pending.push_back({id, std::move(images), std::move(paths)});
    state_->collect();
    state_->pump();
}

void Channel::shutdown() { state_->shutdown(); }

#ifdef MAINWINDOW_START_TEST_SEAM
void Channel::setTestMode(TestMode mode) { state_->testMode = mode; }
bool Channel::testBlocked() const { return state_->io && signaled(state_->io->blocked.get()); }
bool Channel::testReceiptReady() const { return state_->io && signaled(state_->io->reply.get()); }
void Channel::deferTestReceiptNotification() {
    if (state_->receiptEvent) state_->receiptEvent->setEnabled(false);
}
void Channel::setTestExecutable(QString path) { state_->testExecutable = std::move(path); }
void Channel::releaseTestGate() { if (state_->io && state_->io->gate) SetEvent(state_->io->gate.get()); }
qint64 Channel::testProcessId() const { return state_->io ? state_->io->processId : 0; }
#endif
} // namespace ring_png
