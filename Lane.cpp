#include "Lane.h"
#include <algorithm>

namespace TrafficSim {

Lane::Lane(Direction dir) : m_direction(dir) {}

void Lane::addVehicle(const Vehicle& vehicle, ThreadMetric* workerMetric) {
    if (workerMetric) {
        auto tStart = std::chrono::high_resolution_clock::now();
        m_mutex.lock();
        auto tEnd = std::chrono::high_resolution_clock::now();
        double waitMicro = std::chrono::duration<double, std::micro>(tEnd - tStart).count();
        workerMetric->lockWaitCount++;
        workerMetric->totalLockWaitMicroseconds += waitMicro;
        workerMetric->avgLockWaitMicroseconds = workerMetric->totalLockWaitMicroseconds / workerMetric->lockWaitCount;
    } else {
        m_mutex.lock();
    }

    m_queue.push_back(vehicle);
    m_totalArrived++;
    m_mutex.unlock();
}

bool Lane::removeVehicle(Vehicle* outVehicle, ThreadMetric* workerMetric) {
    if (workerMetric) {
        auto tStart = std::chrono::high_resolution_clock::now();
        m_mutex.lock();
        auto tEnd = std::chrono::high_resolution_clock::now();
        double waitMicro = std::chrono::duration<double, std::micro>(tEnd - tStart).count();
        workerMetric->lockWaitCount++;
        workerMetric->totalLockWaitMicroseconds += waitMicro;
        workerMetric->avgLockWaitMicroseconds = workerMetric->totalLockWaitMicroseconds / workerMetric->lockWaitCount;
    } else {
        m_mutex.lock();
    }

    if (m_queue.empty()) {
        m_mutex.unlock();
        return false;
    }

    if (outVehicle) {
        *outVehicle = m_queue.front();
    }
    m_queue.pop_front();
    m_totalServed++;

    m_mutex.unlock();
    return true;
}

int Lane::getQueueLength() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_queue.size());
}

bool Lane::hasEmergencyVehicle() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& v : m_queue) {
        if (v.isEmergency) return true;
    }
    return false;
}

LaneStats Lane::computeStats(double currentSimTime, double starvationThresholdSec,
                             double alpha, double beta, double gamma,
                             ThreadMetric* metric) {
    if (metric) {
        auto tStart = std::chrono::high_resolution_clock::now();
        m_mutex.lock();
        auto tEnd = std::chrono::high_resolution_clock::now();
        double waitMicro = std::chrono::duration<double, std::micro>(tEnd - tStart).count();
        metric->lockWaitCount++;
        metric->totalLockWaitMicroseconds += waitMicro;
        metric->avgLockWaitMicroseconds = metric->totalLockWaitMicroseconds / metric->lockWaitCount;
    } else {
        m_mutex.lock();
    }

    LaneStats stats;
    stats.queueLength = static_cast<int>(m_queue.size());
    stats.totalVehiclesArrived = m_totalArrived;
    stats.totalVehiclesServed = m_totalServed;

    if (m_queue.empty()) {
        m_mutex.unlock();
        return stats;
    }

    double totalWait = 0.0;
    double maxWait = 0.0;
    bool hasEmerg = false;

    for (const auto& v : m_queue) {
        double w = currentSimTime - v.arrivalTimeSec;
        if (w < 0.0) w = 0.0;
        totalWait += w;
        if (w > maxWait) {
            maxWait = w;
        }
        if (v.isEmergency) {
            hasEmerg = true;
        }
    }

    stats.avgWaitTimeSec = totalWait / m_queue.size();
    stats.maxWaitTimeSec = maxWait;
    stats.hasEmergencyVehicle = hasEmerg;

    // Normalization factors for formula
    // Score(i) = alpha * NormDensity + beta * NormWait + gamma * StarvationPenalty
    // NormDensity = min(queueLength / 20.0, 1.0)
    // NormWait = min(avgWaitTime / 30.0, 1.0)
    // StarvationPenalty = (maxWait > starvationThreshold) ? pow((maxWait - starvationThreshold) / 10.0, 1.5) : 0.0
    double normDensity = std::min(stats.queueLength / 20.0, 1.0);
    double normWait = std::min(stats.avgWaitTimeSec / 30.0, 1.0);
    double starvationPenalty = 0.0;
    if (maxWait > starvationThresholdSec) {
        starvationPenalty = std::min((maxWait - starvationThresholdSec) / 15.0, 3.0);
    }

    stats.starvationScore = (alpha * normDensity) + (beta * normWait) + (gamma * starvationPenalty);
    if (hasEmerg) {
        stats.starvationScore += 10.0; // Urgent boost for emergency
    }

    m_mutex.unlock();
    return stats;
}

std::vector<Vehicle> Lane::getVehicleSnapshot(size_t maxCount) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Vehicle> list;
    size_t count = std::min(m_queue.size(), maxCount);
    for (size_t i = 0; i < count; ++i) {
        list.push_back(m_queue[i]);
    }
    return list;
}

void Lane::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.clear();
    m_totalArrived = 0;
    m_totalServed = 0;
    m_accumulatedWaitTimeSec = 0.0;
}

} // namespace TrafficSim
