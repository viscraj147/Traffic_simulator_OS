#include "WatchdogMonitor.h"
#include <chrono>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace TrafficSim {

static uint64_t fileTimeToUint64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

WatchdogMonitor::WatchdogMonitor(TrafficController* controller, LaneWorker* workers[4])
    : m_controller(controller) {
    for (int i = 0; i < 4; ++i) {
        m_workers[i] = workers[i];
    }
    m_metric.threadName = "WatchdogMonitor";
    m_metric.status = ThreadStatus::IDLE;
    m_metric.isHealthy = true;
}

WatchdogMonitor::~WatchdogMonitor() {
    stop();
}

void WatchdogMonitor::start() {
    if (m_running.load()) return;
    m_running.store(true);
    m_thread = std::thread(&WatchdogMonitor::monitorLoop, this);
}

void WatchdogMonitor::stop() {
    if (!m_running.load()) return;
    m_running.store(false);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_metric.status = ThreadStatus::IDLE;
}

void WatchdogMonitor::reset() {
    stop();
    m_activeFault.store(FaultType::NONE);
    m_faultTargetIndex.store(-1);
    m_cpuPercent.store(0.0);
    m_memoryMb.store(0.0);
    m_avgSyncOverheadMicrosec.store(0.0);
    m_metric.status = ThreadStatus::IDLE;
    {
        std::lock_guard<std::mutex> lock(m_faultMutex);
        m_faultHistory.clear();
        m_lastFaultLog.clear();
        m_nextFaultId = 1;
    }
}

void WatchdogMonitor::injectWorkerFailure(int workerIndex, FaultType type) {
    m_activeFault.store(type);
    m_faultTargetIndex.store(workerIndex);

    if (type == FaultType::WORKER_HANG) {
        if (workerIndex >= 0 && workerIndex < 4 && m_workers[workerIndex]) {
            m_workers[workerIndex]->injectHang(true);
        }
    } else if (type == FaultType::CONTROLLER_DELAY) {
        if (m_controller) {
            m_controller->injectProcessingDelay(true, 800);
        }
    } else if (type == FaultType::STALE_UPDATES) {
        if (workerIndex >= 0 && workerIndex < 4 && m_workers[workerIndex]) {
            m_workers[workerIndex]->injectStaleUpdates(true);
        }
    }
}

void WatchdogMonitor::triggerManualRecovery() {
    int target = m_faultTargetIndex.load();
    FaultType type = m_activeFault.load();
    auto now = std::chrono::steady_clock::now();
    double currentSec = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()) / 1000.0;

    if (type == FaultType::WORKER_HANG && target >= 0 && target < 4) {
        if (m_workers[target]) {
            m_workers[target]->recover();
        }
    } else if (type == FaultType::CONTROLLER_DELAY) {
        if (m_controller) {
            m_controller->injectProcessingDelay(false, 0);
            m_controller->exitSafeFallbackMode();
        }
    } else if (type == FaultType::STALE_UPDATES && target >= 0 && target < 4) {
        if (m_workers[target]) {
            m_workers[target]->recover();
        }
    }

    if (m_controller) {
        m_controller->exitSafeFallbackMode();
    }

    m_activeFault.store(FaultType::NONE);
    m_faultTargetIndex.store(-1);

    std::lock_guard<std::mutex> lock(m_faultMutex);
    m_lastFaultLog = "Recovery completed at T=" + std::to_string(currentSec) + "s. System operating in normal mode.";
}

void WatchdogMonitor::sampleOsMetrics() {
#ifdef _WIN32
    FILETIME creationTime, exitTime, kernelTime, userTime;
    if (GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime)) {
        uint64_t kTime = fileTimeToUint64(kernelTime);
        uint64_t uTime = fileTimeToUint64(userTime);
        uint64_t nowTime = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) * 10; // convert to 100ns units

        if (m_lastCpuSampleTime != 0 && nowTime > m_lastCpuSampleTime) {
            uint64_t deltaProcess = (kTime - m_lastKernelTime) + (uTime - m_lastUserTime);
            uint64_t deltaWall = nowTime - m_lastCpuSampleTime;
            SYSTEM_INFO sysInfo;
            GetSystemInfo(&sysInfo);
            double numCores = (sysInfo.dwNumberOfProcessors > 0) ? sysInfo.dwNumberOfProcessors : 1.0;
            double pct = (static_cast<double>(deltaProcess) / static_cast<double>(deltaWall)) * 100.0 / numCores;
            m_cpuPercent.store(std::clamp(pct, 0.0, 100.0));
        }

        m_lastKernelTime = kTime;
        m_lastUserTime = uTime;
        m_lastCpuSampleTime = nowTime;
    }

    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        double mb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        m_memoryMb.store(mb);
    }
#else
    m_cpuPercent.store(2.5);
    m_memoryMb.store(32.0);
#endif

    // Compute average lock synchronization overhead
    double totalWaitMicro = 0.0;
    uint64_t totalCount = 0;
    if (m_controller) {
        totalWaitMicro += m_controller->getMetric().totalLockWaitMicroseconds;
        totalCount += m_controller->getMetric().lockWaitCount;
    }
    for (int i = 0; i < 4; ++i) {
        if (m_workers[i]) {
            totalWaitMicro += m_workers[i]->getMetric().totalLockWaitMicroseconds;
            totalCount += m_workers[i]->getMetric().lockWaitCount;
        }
    }
    if (totalCount > 0) {
        m_avgSyncOverheadMicrosec.store(totalWaitMicro / totalCount);
    }
}

void WatchdogMonitor::checkThreadHeartbeats(double currentSec) {
    const double HEARTBEAT_TIMEOUT_SEC = 1.5; // Missed heartbeat after 1.5s
    
    // Check lane workers
    for (int i = 0; i < 4; ++i) {
        if (!m_workers[i]) continue;
        ThreadMetric& metric = m_workers[i]->getMetric();
        if (metric.lastHeartbeatSec > 0.0 && (currentSec - metric.lastHeartbeatSec) > HEARTBEAT_TIMEOUT_SEC) {
            metric.isHealthy = false;
            metric.status = ThreadStatus::FAILED;

            // Log fault if not already logged
            bool alreadyLogged = false;
            {
                std::lock_guard<std::mutex> lock(m_faultMutex);
                if (!m_faultHistory.empty() &&
                    m_faultHistory.back().affectedComponent == metric.threadName &&
                    (currentSec - m_faultHistory.back().detectionTimestampSec) < 3.0) {
                    alreadyLogged = true;
                }
            }

            if (!alreadyLogged) {
                auto tStartRecover = std::chrono::high_resolution_clock::now();
                // Autonomous recovery action (FR-07): activate safe fallback & attempt worker restart
                if (m_controller) {
                    m_controller->enterSafeFallbackMode("Lane worker " + metric.threadName + " heartbeat timeout");
                }
                
                // Attempt automatic worker recovery if not persistent injection
                if (!m_workers[i]->isHanging()) {
                    m_workers[i]->recover();
                }

                auto tEndRecover = std::chrono::high_resolution_clock::now();
                double recoveryDurationMs = std::chrono::duration<double, std::milli>(tEndRecover - tStartRecover).count();

                FaultRecord rec;
                rec.id = m_nextFaultId++;
                rec.type = FaultType::WORKER_HANG;
                rec.affectedComponent = metric.threadName;
                rec.detectionTimestampSec = currentSec;
                rec.recoveryAction = "Activated SAFE_FALLBACK phase & restarted worker heartbeat pipeline";
                rec.recoveryDurationMs = recoveryDurationMs;
                rec.trafficImpact = "Temporary fixed-time clearance prevents starvation of conflicting lanes";

                std::lock_guard<std::mutex> lock(m_faultMutex);
                m_faultHistory.push_back(rec);
                m_lastFaultLog = "WATCHDOG: Detected missed heartbeat on " + metric.threadName +
                                 " at T=" + std::to_string(currentSec) + "s -> Initiated SAFE_FALLBACK.";
            }
        }
    }
}

void WatchdogMonitor::monitorLoop() {
#ifdef _WIN32
    m_metric.osThreadId = static_cast<uint32_t>(GetCurrentThreadId());
#endif

    m_metric.status = ThreadStatus::RUNNING;

    while (m_running.load()) {
        auto now = std::chrono::steady_clock::now();
        double currentSec = static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()) / 1000.0;
        
        m_metric.lastHeartbeatSec = currentSec;

        sampleOsMetrics();
        checkThreadHeartbeats(currentSec);

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    m_metric.status = ThreadStatus::IDLE;
}

std::vector<FaultRecord> WatchdogMonitor::getFaultHistory() {
    std::lock_guard<std::mutex> lock(m_faultMutex);
    return m_faultHistory;
}

std::string WatchdogMonitor::getLastFaultLog() {
    std::lock_guard<std::mutex> lock(m_faultMutex);
    return m_lastFaultLog;
}

} // namespace TrafficSim
