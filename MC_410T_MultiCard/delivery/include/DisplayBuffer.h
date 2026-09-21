#pragma once
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>
#include "DataTypes.h"

// ============================================================
// DisplayBuffer  带互斥锁的单槽最新帧缓冲区
//
// 写者（DataProcessor 处理线程）调用 update()：用新帧覆盖 m_latest。
// 读者（Qt UI 主线程）调用 tryRead()：若有新帧则取走并返回 true，否则返回 false。
// "新帧"由 m_hasNew 原子标志判断，彻底避免版本号/槽索引的竞态问题。
// update() 和 tryRead() 均持有 m_mutex，锁时间极短（vector assign，微秒级）。
// UI 刷新率 30fps，远低于 DataProcessor 产生速率，不会阻塞处理线程。
// ============================================================
class DisplayBuffer {
public:
    // 传递给 UI 的快照（已完成 float32 → double 转换，全分辨率显示数据）
    struct Snapshot {
        bool     valid        = false;
        uint16_t triggerSeq   = 0;
        int      sampleCount  = 0;
        uint64_t timestamp_ms = 0;
        std::vector<double> phaseA;  // A 通道差分相位
        std::vector<double> phaseB;
        std::vector<double> freqA;   // A 通道瞬时频率（kHz）
        std::vector<double> freqB;
    };

    // 全分辨率频率数据快照（用于成像馈送，float32 原始数据）
    struct FullResSnapshot {
        bool     valid        = false;
        uint16_t triggerSeq   = 0;
        int      sampleCount  = 0;
        uint64_t timestamp_ms = 0;
        std::vector<float> freqA;    // 全分辨率 A 通道瞬时频率
        std::vector<float> freqB;    // 全分辨率 B 通道瞬时频率
    };

    DisplayBuffer() = default;

    // 写者调用：用最新帧覆盖显示缓冲（DataProcessor 线程）
    void update(const TriggerGroupPtr& group) {
        Snapshot newSnap;
        newSnap.valid        = true;
        newSnap.triggerSeq   = group->triggerSeq;
        newSnap.sampleCount  = group->sampleCount;
        newSnap.timestamp_ms = group->timestamp_ms;

        auto convert = [](const std::vector<float>& src, std::vector<double>& dst) {
            dst.resize(src.size());
            for (size_t i = 0; i < src.size(); ++i) dst[i] = static_cast<double>(src[i]);
        };
        convert(group->phaseA_display, newSnap.phaseA);
        convert(group->phaseB_display, newSnap.phaseB);
        convert(group->freqA_display,  newSnap.freqA);
        convert(group->freqB_display,  newSnap.freqB);

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_latest = std::move(newSnap);
        }
        m_hasNew.store(true, std::memory_order_release);
    }

    // 写者调用：存储全分辨率频率数据（DataProcessor 线程，供成像使用）
    void updateFullRes(const TriggerGroupPtr& group) {
        FullResSnapshot snap;
        snap.valid        = true;
        snap.triggerSeq   = group->triggerSeq;
        snap.sampleCount  = group->sampleCount;
        snap.timestamp_ms = group->timestamp_ms;
        snap.freqA        = group->freqA;   // 直接拷贝全分辨率 vector<float>
        snap.freqB        = group->freqB;
        {
            std::lock_guard<std::mutex> lk(m_fullMutex);
            m_fullLatest = std::move(snap);
        }
    }

    // 读者调用：取最新帧（Qt UI 主线程）
    // 返回 true  → snap 已填充最新数据
    // 返回 false → 自上次成功读取后尚无新帧
    bool tryRead(Snapshot& snap) {
        if (!m_hasNew.load(std::memory_order_acquire)) return false;
        m_hasNew.store(false, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(m_mutex);
        snap = m_latest;
        return snap.valid;
    }

    // 非破坏性读取最新帧（不消费 hasNew 标志，不阻止后续 tryRead 获取相同数据）
    // 用于成像等辅助消费场景，避免与主显示拉取冲突
    bool peekLatest(Snapshot& snap) {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_latest.valid) return false;
        snap = m_latest;
        return true;
    }

    // 非破坏性读取全分辨率频率数据（供成像馈送）
    bool peekLatestFull(FullResSnapshot& snap) {
        std::lock_guard<std::mutex> lk(m_fullMutex);
        if (!m_fullLatest.valid) return false;
        snap = m_fullLatest;
        return true;
    }

    // 重置
    void reset() {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_latest = Snapshot{};
        m_hasNew.store(false, std::memory_order_relaxed);
        std::lock_guard<std::mutex> flk(m_fullMutex);
        m_fullLatest = FullResSnapshot{};
    }

private:
    std::mutex            m_mutex;
    Snapshot              m_latest;
    std::atomic<bool>     m_hasNew{false};
    std::mutex            m_fullMutex;
    FullResSnapshot       m_fullLatest;
};
