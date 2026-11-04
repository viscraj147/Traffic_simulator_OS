#pragma once

#include "Common.h"
#include "TrafficController.h"
#include "LaneWorker.h"
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <mutex>

namespace TrafficSim {

struct FaultRecord {
    uint64_t id;
    FaultType type;
    std::string affectedComponent;
    double detectionTimestampSec;
    std::string recoveryAction;
    double recoveryDurationMs;
    std::string trafficImpact;
};

class WatchdogMonitor {
public:
    WatchdogMonitor(TrafficController* controller, LaneWorker* workers[4]);
    ~WatchdogMonitor();

    void start();
    void stop();
    void reset();

    // Injected fault controls
    void injectWorkerFailure(int workerIndex, FaultType type);
    void triggerManualRecovery();

    // Metrics getters
    double getCpuUsagePercent() const { return m_cpuPercent.load(); }
    double getMemoryUsageMb() const { return m_memoryMb.load(); }
    double getAvgSyncOverheadMicrosec() const { return m_avgSyncOverheadMicrosec.load(); }
    std::vector<FaultRecord> getFaultHistory();
    ThreadMetric& getMetric() { return m_metric; }

    FaultType getActiveFault() const { return m_activeFault.load(); }
    int getFaultTargetIndex() const { return m_faultTargetIndex.load(); }
    std::string getLastFaultLog();

private:
    void monitorLoop();
    void sampleOsMetrics();
    void checkThreadHeartbeats(double currentSec);

    TrafficController* m_controller;
    LaneWorker* m_workers[4];

    std::thread m_thread;
    std::atomic<bool> m_running{false};

    ThreadMetric m_metric;
    std::atomic<double> m_cpuPercent{0.0};
    std::atomic<double> m_memoryMb{0.0};
    std::atomic<double> m_avgSyncOverheadMicrosec{0.0};

    // Fault state
    std::atomic<FaultType> m_activeFault{FaultType::NONE};
    std::atomic<int> m_faultTargetIndex{-1};
    std::string m_lastFaultLog;
    std::mutex m_faultMutex;
    std::vector<FaultRecord> m_faultHistory;
    uint64_t m_nextFaultId{1};

    // OS CPU calculation state
    uint64_t m_lastCpuSampleTime{0};
    uint64_t m_lastKernelTime{0};
    uint64_t m_lastUserTime{0};
};

} // namespace TrafficSim
