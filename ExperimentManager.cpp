#include "ExperimentManager.h"
#include "Lane.h"
#include "LaneWorker.h"
#include "TrafficController.h"
#include "WatchdogMonitor.h"
#include "MessageQueue.h"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>

namespace TrafficSim {

ExperimentManager::ExperimentManager() {}

void ExperimentManager::addResult(const BenchmarkResult& res) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_results.push_back(res);
}

std::vector<BenchmarkResult> ExperimentManager::getResults() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_results;
}

std::string ExperimentManager::exportToCsvString() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::ostringstream ss;
    ss << "Scenario,Algorithm,SyncStrategy,DurationSec,TotalServed,ThroughputPerMin,"
       << "AvgWaitSec,MaxWaitSec,JainsFairnessIndex,StarvationEvents,"
       << "AvgCpuPercent,PeakMemoryMb,AvgControllerLatencyMicrosec,AvgSyncOverheadMicrosec,DeadlineMisses\n";

    for (const auto& r : m_results) {
        ss << "\"" << r.testScenarioName << "\","
           << "\"" << r.algorithmName << "\","
           << "\"" << r.syncStrategyName << "\","
           << std::fixed << std::setprecision(2)
           << r.durationSec << ","
           << r.totalServed << ","
           << r.throughputPerMin << ","
           << r.avgWaitSec << ","
           << r.maxWaitSec << ","
           << std::setprecision(4)
           << r.jainsFairnessIndex << ","
           << r.starvationEvents << ","
           << std::setprecision(2)
           << r.avgCpuPercent << ","
           << r.peakMemoryMb << ","
           << r.avgControllerLatencyMicrosec << ","
           << r.avgSyncOverheadMicrosec << ","
           << r.deadlineMisses << "\n";
    }

    return ss.str();
}

void ExperimentManager::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_results.clear();
}

// Executes a benchmark trial for a given algorithm & sync strategy
static BenchmarkResult runTrial(const std::string& scenarioName,
                                AlgorithmType alg,
                                SyncStrategy sync,
                                double arrivalRates[4],
                                double testDurationSec) {
    Lane* lanes[4];
    LaneWorker* workers[4];
    MessageQueue<LaneMessage> msgQueue(500);

    for (int i = 0; i < 4; ++i) {
        lanes[i] = new Lane(static_cast<Direction>(i));
        workers[i] = new LaneWorker(static_cast<Direction>(i), lanes[i],
                                    (sync == SyncStrategy::MESSAGE_QUEUE) ? &msgQueue : nullptr);
    }

    TrafficController controller(lanes, workers, (sync == SyncStrategy::MESSAGE_QUEUE) ? &msgQueue : nullptr);
    WatchdogMonitor watchdog(&controller, workers);

    SimulationConfig cfg;
    cfg.algorithm = alg;
    cfg.syncStrategy = sync;
    cfg.timeSpeedMultiplier = 20.0; // accelerated 20x for rapid benchmark execution
    for (int i = 0; i < 4; ++i) {
        cfg.arrivalRatePerMin[i] = arrivalRates[i];
    }
    controller.setConfig(cfg);

    // Start all concurrent worker threads
    for (int i = 0; i < 4; ++i) workers[i]->start();
    controller.start();
    watchdog.start();

    // Run test for target duration (scaled)
    int sleepMs = static_cast<int>((testDurationSec / cfg.timeSpeedMultiplier) * 1000.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));

    // Capture snapshot at conclusion
    IntersectionSnapshot snap = controller.getSnapshot();

    // Stop threads safely
    watchdog.stop();
    controller.stop();
    for (int i = 0; i < 4; ++i) workers[i]->stop();

    BenchmarkResult res;
    res.testScenarioName = scenarioName;
    res.algorithmName = algorithmToString(alg);
    res.syncStrategyName = syncStrategyToString(sync);
    res.durationSec = testDurationSec;
    res.totalServed = snap.totalVehiclesProcessed;
    res.throughputPerMin = (res.totalServed / testDurationSec) * 60.0;
    res.avgWaitSec = snap.overallAvgWaitSec;
    res.maxWaitSec = snap.overallMaxWaitSec;
    res.jainsFairnessIndex = snap.jainsFairnessIndex;
    res.starvationEvents = snap.starvationViolationsCount;
    res.avgCpuPercent = watchdog.getCpuUsagePercent();
    res.peakMemoryMb = watchdog.getMemoryUsageMb();
    res.avgControllerLatencyMicrosec = snap.controllerDecisionLatencyMicrosec;
    res.avgSyncOverheadMicrosec = watchdog.getAvgSyncOverheadMicrosec();
    res.deadlineMisses = snap.deadlineMissCount;

    for (int i = 0; i < 4; ++i) {
        delete workers[i];
        delete lanes[i];
    }

    return res;
}

std::vector<BenchmarkResult> ExperimentManager::runComprehensiveBenchmark() {
    std::vector<BenchmarkResult> batch;

    // Scenario 1: Low Uniform Traffic (15 vpm all approaches)
    {
        double rates[4] = {15.0, 15.0, 15.0, 15.0};
        batch.push_back(runTrial("1. Low Uniform Traffic", AlgorithmType::BASELINE_A_FIXED_TIME, SyncStrategy::SHARED_MUTEX, rates, 30.0));
        batch.push_back(runTrial("1. Low Uniform Traffic", AlgorithmType::BASELINE_B_ADAPTIVE, SyncStrategy::SHARED_MUTEX, rates, 30.0));
        batch.push_back(runTrial("1. Low Uniform Traffic", AlgorithmType::PROPOSED_C_FAIRNESS_FAULT_TOLERANT, SyncStrategy::SHARED_MUTEX, rates, 30.0));
    }

    // Scenario 2: Asymmetric Traffic (North & East heavy, South & West light)
    {
        double rates[4] = {40.0, 10.0, 35.0, 8.0};
        batch.push_back(runTrial("2. Asymmetric Traffic", AlgorithmType::BASELINE_A_FIXED_TIME, SyncStrategy::SHARED_MUTEX, rates, 30.0));
        batch.push_back(runTrial("2. Asymmetric Traffic", AlgorithmType::BASELINE_B_ADAPTIVE, SyncStrategy::SHARED_MUTEX, rates, 30.0));
        batch.push_back(runTrial("2. Asymmetric Traffic", AlgorithmType::PROPOSED_C_FAIRNESS_FAULT_TOLERANT, SyncStrategy::SHARED_MUTEX, rates, 30.0));
    }

    // Scenario 3: Starvation Stress Test (West neglected with 3 vpm, others heavy)
    {
        double rates[4] = {45.0, 45.0, 40.0, 3.0};
        batch.push_back(runTrial("3. Starvation Stress", AlgorithmType::BASELINE_A_FIXED_TIME, SyncStrategy::SHARED_MUTEX, rates, 30.0));
        batch.push_back(runTrial("3. Starvation Stress", AlgorithmType::BASELINE_B_ADAPTIVE, SyncStrategy::SHARED_MUTEX, rates, 30.0));
        batch.push_back(runTrial("3. Starvation Stress", AlgorithmType::PROPOSED_C_FAIRNESS_FAULT_TOLERANT, SyncStrategy::SHARED_MUTEX, rates, 30.0));
    }

    // Scenario 4: Synchronization Comparison (Mutex vs Message Queue under Heavy 50 vpm Load)
    {
        double rates[4] = {50.0, 50.0, 50.0, 50.0};
        batch.push_back(runTrial("4. Sync Strategy Comparison", AlgorithmType::PROPOSED_C_FAIRNESS_FAULT_TOLERANT, SyncStrategy::SHARED_MUTEX, rates, 30.0));
        batch.push_back(runTrial("4. Sync Strategy Comparison", AlgorithmType::PROPOSED_C_FAIRNESS_FAULT_TOLERANT, SyncStrategy::MESSAGE_QUEUE, rates, 30.0));
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& b : batch) {
            m_results.push_back(b);
        }
    }

    return batch;
}

} // namespace TrafficSim
