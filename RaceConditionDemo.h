#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace TrafficSim {

struct RaceDemoResult {
    bool synchronized;
    uint32_t numThreads;
    uint64_t iterationsPerThread;
    uint64_t expectedTotal;
    uint64_t actualTotal;
    int64_t lostUpdates;
    double corruptionRatePercent;
    double executionDurationMs;
    std::string analysisSummary;
};

class RaceConditionDemo {
public:
    static RaceDemoResult runDemo(bool synchronized, uint32_t numThreads = 4, uint64_t iterationsPerThread = 100000);
};

} // namespace TrafficSim
