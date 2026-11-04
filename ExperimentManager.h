#pragma once

#include "Common.h"
#include <vector>
#include <string>
#include <mutex>

namespace TrafficSim {

class ExperimentManager {
public:
    ExperimentManager();

    // Run a comprehensive automated multi-scenario benchmark
    std::vector<BenchmarkResult> runComprehensiveBenchmark();

    // Add individual test result
    void addResult(const BenchmarkResult& res);

    // Retrieve all recorded benchmark runs
    std::vector<BenchmarkResult> getResults();

    // Export to CSV format string
    std::string exportToCsvString();

    // Clear history
    void clear();

private:
    std::mutex m_mutex;
    std::vector<BenchmarkResult> m_results;
};

} // namespace TrafficSim
