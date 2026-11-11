#pragma once
#include "RingInputTransport.h"

namespace ring_input {
constexpr wchar_t kNativeMappingName[] = L"Local\\MC_410T_RingShm_v3";
constexpr wchar_t kOutputMutexName[] = L"Local\\MC_410T_RingShm_Output_v3";

// Windows page-file mapping; never attach implicitly when creation collides.
// The caller validates header, protocol, logical size and configured dimensions
// before accessing data. The reported capacity may be rounded up by Windows.
class NativeMapping {
public:
    NativeMapping() = default;
    ~NativeMapping() { close(); }
    NativeMapping(const NativeMapping &) = delete;
    NativeMapping &operator=(const NativeMapping &) = delete;
    Result create(size_t bytes, const wchar_t *name = kNativeMappingName) {
#ifdef _WIN32
        if (handle_ || !bytes || !name) return Result::Invalid;
        const uint64_t n = bytes;
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                      DWORD(n >> 32), DWORD(n), name);
        if (!h) return Result::OsError;
        const DWORD status = GetLastError();
        if (status == ERROR_ALREADY_EXISTS) { CloseHandle(h); return Result::Busy; }
        return map(h);
#else
        (void)bytes; (void)name; return Result::Unavailable;
#endif
    }
    Result attach(const wchar_t *name = kNativeMappingName) {
#ifdef _WIN32
        if (handle_ || !name) return Result::Invalid;
        HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        if (!h) return Result::OsError;
        return map(h);
#else
        (void)name; return Result::Unavailable;
#endif
    }
    void close() {
#ifdef _WIN32
        if (data_) UnmapViewOfFile(data_);
        if (handle_) CloseHandle(handle_);
        data_ = nullptr; handle_ = nullptr; capacity_ = 0;
#endif
    }
    void *data() const { return data_; }
    size_t capacity() const { return capacity_; }
private:
#ifdef _WIN32
    Result map(HANDLE h) {
        void *p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!p) { CloseHandle(h); return Result::OsError; }
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(p, &info, sizeof(info)) || info.AllocationBase != p) {
            UnmapViewOfFile(p); CloseHandle(h); return Result::OsError;
        }
        handle_ = h; data_ = p; capacity_ = info.RegionSize;
        return Result::Ok;
    }
    HANDLE handle_ = nullptr;
#endif
    void *data_ = nullptr;
    size_t capacity_ = 0;
};

// Output lock is independent from the input lock. On Result::Abandoned the
// caller owns the mutex: discard/invalidate the output frame while holding it,
// then call unlock(); never modify input state. On Timeout no data was locked.
} // namespace ring_input
