#pragma once
#include <atomic>
#include <cstdint>
#include <cassert>

// ============================================================
// ObjectPool<T, N>  无锁对象池（header-only 模板）
//
// 实现：tagged pointer（高32位=版本号，低32位=池内索引），ABA-safe
// T 的大小不限，适合 DataPacket（1.5KB）等中等大小对象
// ============================================================

template<typename T, int N>
class ObjectPool {
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");
    static_assert(N > 0 && N <= 65536, "N must be in (0, 65536]");

public:
    ObjectPool() {
        // 初始化：将所有索引推入空闲栈
        for (int i = N - 1; i >= 0; --i) {
            m_next[i] = i + 1 < N ? i + 1 : INVALID_IDX;
        }
        m_freeHead.store(packTag(0, 0), std::memory_order_relaxed);
    }

    // 从池中获取一个对象，失败时返回 nullptr（池满）
    T* acquire() {
        uint64_t old = m_freeHead.load(std::memory_order_acquire);
        for (;;) {
            uint32_t idx = lowIdx(old);
            if (idx == INVALID_IDX) return nullptr;  // 池已满
            uint32_t ver = highVer(old);
            uint64_t next = packTag(m_next[idx], ver + 1);
            if (m_freeHead.compare_exchange_weak(old, next,
                    std::memory_order_release, std::memory_order_acquire)) {
                return &m_objects[idx];
            }
        }
    }

    // 将对象归还给池（ptr 必须是 acquire() 返回的指针）
    void release(T* ptr) {
        if (!ptr) return;
        uint32_t idx = static_cast<uint32_t>(ptr - m_objects);
        assert(idx < static_cast<uint32_t>(N));
        uint64_t old = m_freeHead.load(std::memory_order_acquire);
        for (;;) {
            m_next[idx] = lowIdx(old);
            uint64_t next = packTag(idx, highVer(old) + 1);
            if (m_freeHead.compare_exchange_weak(old, next,
                    std::memory_order_release, std::memory_order_acquire)) {
                return;
            }
        }
    }

    // 返回对象的池内索引（0-based）
    int indexOf(const T* ptr) const {
        return static_cast<int>(ptr - m_objects);
    }

    static constexpr int capacity() { return N; }

private:
    static constexpr uint32_t INVALID_IDX = 0xFFFFFFFFu;

    static uint64_t packTag(uint32_t idx, uint32_t ver) {
        return (static_cast<uint64_t>(ver) << 32) | idx;
    }
    static uint32_t lowIdx(uint64_t v) { return static_cast<uint32_t>(v & 0xFFFFFFFFu); }
    static uint32_t highVer(uint64_t v) { return static_cast<uint32_t>(v >> 32); }

    alignas(64) T          m_objects[N];      // 对象数组（缓存行对齐）
    uint32_t               m_next[N];         // 空闲链表：next[i] = 下一个空闲索引
    std::atomic<uint64_t>  m_freeHead{0};     // tagged pointer：{version, index}
};
