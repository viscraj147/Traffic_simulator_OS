#pragma once

#include "Common.h"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace TrafficSim {

struct LaneMessage {
    Direction direction;
    LaneStats stats;
    double timestampSec;
    bool isHeartbeat;
};

template<typename T>
class MessageQueue {
public:
    MessageQueue(size_t maxCapacity = 1000) : m_capacity(maxCapacity) {}

    void push(const T& item, ThreadMetric* metric = nullptr) {
        auto tStart = std::chrono::high_resolution_clock::now();
        std::unique_lock<std::mutex> lock(m_mutex);
        if (metric) {
            auto tEnd = std::chrono::high_resolution_clock::now();
            double waitMicro = std::chrono::duration<double, std::micro>(tEnd - tStart).count();
            metric->lockWaitCount++;
            metric->totalLockWaitMicroseconds += waitMicro;
            metric->avgLockWaitMicroseconds = metric->totalLockWaitMicroseconds / metric->lockWaitCount;
        }

        m_notFull.wait(lock, [this]() { return m_queue.size() < m_capacity || m_stopped; });
        if (m_stopped) return;

        m_queue.push(item);
        lock.unlock();
        m_notEmpty.notify_one();
    }

    bool pop(T& item, std::chrono::milliseconds timeout = std::chrono::milliseconds(100), ThreadMetric* metric = nullptr) {
        auto tStart = std::chrono::high_resolution_clock::now();
        std::unique_lock<std::mutex> lock(m_mutex);
        if (metric) {
            auto tEnd = std::chrono::high_resolution_clock::now();
            double waitMicro = std::chrono::duration<double, std::micro>(tEnd - tStart).count();
            metric->lockWaitCount++;
            metric->totalLockWaitMicroseconds += waitMicro;
            metric->avgLockWaitMicroseconds = metric->totalLockWaitMicroseconds / metric->lockWaitCount;
        }

        if (!m_notEmpty.wait_for(lock, timeout, [this]() { return !m_queue.empty() || m_stopped; })) {
            return false;
        }

        if (m_stopped && m_queue.empty()) return false;

        item = m_queue.front();
        m_queue.pop();
        lock.unlock();
        m_notFull.notify_one();
        return true;
    }

    bool tryPop(T& item) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) return false;
        item = m_queue.front();
        m_queue.pop();
        m_notFull.notify_one();
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopped = true;
        }
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::queue<T> empty;
        std::swap(m_queue, empty);
        m_stopped = false;
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::condition_variable m_notFull;
    std::queue<T> m_queue;
    size_t m_capacity;
    bool m_stopped{false};
};

} // namespace TrafficSim
