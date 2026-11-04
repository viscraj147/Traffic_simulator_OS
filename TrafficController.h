#pragma once

#include "Common.h"
#include "Lane.h"
#include "LaneWorker.h"
#include "MessageQueue.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <vector>

namespace TrafficSim {

class TrafficController {
public:
    TrafficController(Lane* lanes[4], LaneWorker* workers[4], MessageQueue<LaneMessage>* msgQueue = nullptr);
    ~TrafficController();

    void start();
    void stop();
    void reset();

    // Configuration
    void setConfig(const SimulationConfig& cfg);
    SimulationConfig getConfig() const;

    // Emergency vehicle coordination
    void requestEmergencyPreemption(Direction dir, const std::string& vehicleInfo);

    // Fault injection
    void injectProcessingDelay(bool delayActive, int delayMs = 500);
    void enterSafeFallbackMode(const std::string& reason);
    void exitSafeFallbackMode();

    // Telemetry snapshots
    IntersectionSnapshot getSnapshot();
    ThreadMetric& getMetric() { return m_metric; }
    SignalPhase getCurrentPhase() const { return m_currentPhase.load(); }
    SignalColor getSignal(Direction dir) const;

private:
    void controllerLoop();
    void decideNextPhaseAndDuration(SignalPhase& outNextPhase, double& outDurationSec);
    void applySignalsForPhase(SignalPhase phase);
    void updateLaneWorkersSignals();

    Lane* m_lanes[4];
    LaneWorker* m_workers[4];
    MessageQueue<LaneMessage>* m_msgQueue;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_paused{false};

    mutable std::mutex m_configMutex;
    SimulationConfig m_config;

    std::atomic<SignalPhase> m_currentPhase{SignalPhase::NORTH_SOUTH_GREEN};
    std::atomic<double> m_phaseStartTimeSec{0.0};
    std::atomic<double> m_phaseDurationSec{10.0};

    // Emergency state
    std::atomic<bool> m_emergencyActive{false};
    std::atomic<Direction> m_emergencyDir{Direction::NORTH};
    double m_emergencyRequestTimeSec{0.0};
    double m_emergencyGrantTimeSec{0.0};
    std::string m_emergencyLog;

    // Fault & fallback state
    std::atomic<bool> m_delayInjected{false};
    std::atomic<int> m_injectedDelayMs{0};
    std::atomic<bool> m_safeFallbackActive{false};
    std::string m_fallbackReason;

    // Metrics & Telemetry
    ThreadMetric m_metric;
    std::atomic<double> m_decisionLatencyMicrosec{0.0};
    std::atomic<uint64_t> m_deadlineMissCount{0};
    std::atomic<uint64_t> m_starvationCount{0};
    std::atomic<uint64_t> m_totalProcessed{0};

    // Stored latest message queue stats if in MessageQueue mode
    LaneStats m_cachedMqStats[4];
    std::mutex m_mqStatsMutex;
};

} // namespace TrafficSim
