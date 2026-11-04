#include "RaceConditionDemo.h"
#include <thread>
#include <mutex>
#include <chrono>

namespace TrafficSim {

RaceDemoResult RaceConditionDemo::runDemo(bool synchronized, uint32_t numThreads, uint64_t iterationsPerThread) {
    RaceDemoResult result;
    result.synchronized = synchronized;
    result.numThreads = numThreads;
    result.iterationsPerThread = iterationsPerThread;
    result.expectedTotal = static_cast<uint64_t>(numThreads) * iterationsPerThread;

    volatile uint64_t unsafeCounter = 0;
    uint64_t safeCounter = 0;
    std::mutex mtx;

    auto tStart = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> workers;
    workers.reserve(numThreads);

    for (uint32_t t = 0; t < numThreads; ++t) {
        workers.emplace_back([&, synchronized]() {
            for (uint64_t i = 0; i < iterationsPerThread; ++i) {
                if (synchronized) {
                    std::lock_guard<std::mutex> lock(mtx);
                    safeCounter++;
                } else {
                    // Non-atomic read-modify-write induces lost updates
                    uint64_t temp = unsafeCounter;
                    // Introduce a tiny compiler barrier / volatile load to guarantee assembly doesn't optimize it out
                    unsafeCounter = temp + 1;
                }
            }
        });
    }

    for (auto& w : workers) {
        if (w.joinable()) {
            w.join();
        }
    }

    auto tEnd = std::chrono::high_resolution_clock::now();
    result.executionDurationMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

    if (synchronized) {
        result.actualTotal = safeCounter;
        result.lostUpdates = 0;
        result.corruptionRatePercent = 0.0;
        result.analysisSummary = "Mutual exclusion guaranteed: all " + std::to_string(result.expectedTotal) +
                                 " updates were serialized without data races. 0 lost updates.";
    } else {
        result.actualTotal = unsafeCounter;
        result.lostUpdates = static_cast<int64_t>(result.expectedTotal) - static_cast<int64_t>(result.actualTotal);
        if (result.lostUpdates < 0) result.lostUpdates = 0;
        result.corruptionRatePercent = (static_cast<double>(result.lostUpdates) / result.expectedTotal) * 100.0;
        result.analysisSummary = "Race condition confirmed: " + std::to_string(result.lostUpdates) +
                                 " updates were silently dropped (" +
                                 std::to_string(result.corruptionRatePercent) +
                                 "% corruption) due to unsynchronized concurrent read-modify-write cycles.";
    }

    return result;
}

} // namespace TrafficSim
