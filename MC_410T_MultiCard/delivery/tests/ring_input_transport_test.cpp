#include "RingNativeMapping.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); } } while (false)
#include <vector>
#include <thread>

int main()
{
#ifdef _WIN32
    ring_input::NativeMapping owner, peer, duplicate;
    constexpr wchar_t mappingName[] = L"Local\\RingNativeMapping_FocusedTest_20261111";
    CHECK(owner.create(4096, mappingName) == ring_input::Result::Ok);
    CHECK(duplicate.create(4096, mappingName) == ring_input::Result::Busy);
    CHECK(peer.attach(mappingName) == ring_input::Result::Ok);
    CHECK(owner.capacity() >= 4096 && peer.capacity() >= 4096);
    static_cast<unsigned char *>(owner.data())[99] = 0x5a;
    CHECK(static_cast<unsigned char *>(peer.data())[99] == 0x5a);
    const size_t bytes = ringImagingShmTotalSizeV3(4, 1, 2, 1);
    std::vector<uint64_t> storage((bytes + 4095) / 8);
    auto *h = reinterpret_cast<RingImagingShmHeader *>(storage.data());
    h->magic = 0x52494E47u; h->block_size = 4; h->frame_size = 1;
    h->alines = 2; h->display_frame_size = 1;
    ring_input::Mutex mutex(L"Local\\RingInputTransport_FocusedTest_20261111");
    using ring_input::Result;
    CHECK(ring_input::initialize(mutex, h, bytes, 11, 0) == Result::Ok);
    HANDLE locked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread holder([&] { ring_input::Mutex other(L"Local\\RingInputTransport_FocusedTest_20261111");
        CHECK(other.lock(0) == Result::Ok); SetEvent(locked); WaitForSingleObject(release, INFINITE); other.unlock(); });
    CHECK(WaitForSingleObject(locked, 5000) == WAIT_OBJECT_0);
    CHECK(mutex.lock(0) == Result::Timeout);
    SetEvent(release); holder.join(); CloseHandle(locked); CloseHandle(release);
    float a[] = {1,2,3,4}, b[] = {5,6,7,8}, angles[] = {0.1f,0.2f};
    uint8_t channels[] = {3,4}, outputChannels[2]{};
    float output[4]{}, outputAngles[2]{};
    ring_input::Identity first{11, 1, 8, 9, 10, 17, 1, true, 3};
    ring_input::Identity second{11, 2, 8, 9, 11, 18, 0, false, 4}, observed{};
    CHECK(ring_input::publish(mutex,h,bytes,first,a,angles,channels,0) == Result::Ok);
    CHECK(ring_input::publish(mutex,h,bytes,second,b,angles,channels,0) == Result::Busy);
    CHECK(ring_input::copyAndAcknowledge(mutex,h,bytes,11,2,output,outputAngles,outputChannels,observed,0) == Result::Mismatch);
    CHECK(ring_input::copyAndAcknowledge(mutex,h,bytes,11,1,output,outputAngles,outputChannels,observed,0) == Result::Ok);
    CHECK(output[0] == 1 && output[3] == 4 && outputAngles[1] == angles[1] && outputChannels[1] == 4);
    CHECK(observed.measurementSession == 8 && observed.roundGeneration == 9 && observed.blockSeq == 17 && observed.roundBlockIndex == 3 && observed.sourceRoundComplete);
    bool copied = false;
    CHECK(ring_input::acknowledged(mutex,h,bytes,11,1,copied,0) == Result::Ok && copied);
    CHECK(ring_input::copyAndAcknowledge(mutex,h,bytes,11,1,output,outputAngles,outputChannels,observed,0) == Result::Mismatch);
    CHECK(ring_input::publish(mutex,h,bytes,second,b,angles,channels,0) == Result::Ok);
    CHECK(ring_input::reset(mutex,h,bytes,12,0) == Result::Ok);
    CHECK(ring_input::copyAndAcknowledge(mutex,h,bytes,11,2,output,outputAngles,outputChannels,observed,0) == Result::Mismatch);
    first.epoch = 12;
    CHECK(ring_input::publish(mutex,h,bytes,first,a,angles,channels,0) == Result::Ok);
    std::thread deadOwner([] { HANDLE m = CreateMutexW(nullptr, FALSE, L"Local\\RingInputTransport_FocusedTest_20261111");
        CHECK(WaitForSingleObject(m, 0) == WAIT_OBJECT_0); CloseHandle(m); });
    deadOwner.join();
    CHECK(ring_input::copyAndAcknowledge(mutex,h,bytes,12,1,output,outputAngles,outputChannels,observed,0) == Result::Abandoned);
    CHECK(ring_input::copyAndAcknowledge(mutex,h,bytes,12,1,output,outputAngles,outputChannels,observed,0) == Result::Mismatch);
    CHECK(ring_input::reset(mutex,h,bytes,13,0) == Result::Ok);
    CHECK(ring_input::reset(mutex,h,bytes,13,0) == Result::Invalid);
    ring_input::View view;
    CHECK(!ring_input::layout(h, bytes - 1, true, view));
    CHECK(ring_input::layout(h, storage.size() * 8, true, view));
    h->version = 2;
    CHECK(!ring_input::layout(h, bytes, true, view));
    h->version = kRingImagingShmVersion3;
    CHECK(ring_input::layout(h, bytes, true, view));
    view.meta->logical_size++;
    CHECK(!ring_input::layout(h, bytes, true, view));
    view.meta->logical_size--;
    first.epoch = 13;
    CHECK(ring_input::publish(mutex,h,bytes,first,a,angles,channels,0) == Result::Ok);
    CHECK(ring_input::withdraw(mutex,h,bytes,13,1,0) == Result::Ok);
    CHECK(ring_input::publish(mutex,h,bytes,first,a,angles,channels,0) == Result::Invalid);
    CHECK(ring_input::acknowledged(mutex,h,bytes,13,1,copied,0) == Result::Ok && !copied);
#else
    ring_input::Mutex mutex;
    CHECK(mutex.lock(0) == ring_input::Result::Unavailable);
#endif
}
