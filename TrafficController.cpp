#include "TrafficController.h"
#include <chrono>
#include <algorithm>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#endif

namespace TrafficSim {

TrafficController::TrafficController(Lane* lanes[4], LaneWorker* workers[4], MessageQueue<LaneMessage>* msgQueue)
    : m_msgQueue(msgQueue) {
    for (int i = 0; i < 4; ++i) {
        m_lanes[i] = lanes[i];
        m_workers[i] = workers[i];
    }
    m_metric.threadName = "TrafficController";
    m_metric.status = ThreadStatus::IDLE;
    m_metric.isHealthy = true;
}

TrafficController::~TrafficController() {
    stop();
}

void TrafficController::start() {
    if (m_running.load()) return;
    m_running.store(true);
    m_thread = std::thread(&TrafficController::controllerLoop, this);
}

void TrafficController::stop() {
    if (!m_running.load()) return;
    m_running.store(false);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_metric.status = ThreadStatus::IDLE;
}

void TrafficController::reset() {
    stop();
    m_emergencyActive.store(false);
    m_delayInjected.store(false);
    m_safeFallbackActive.store(false);
    m_currentPhase.store(SignalPhase::NORTH_SOUTH_GREEN);
    m_decisionLatencyMicrosec.store(0.0);
    m_deadlineMissCount.store(0);
    m_starvationCount.store(0);
    m_totalProcessed.store(0);
    m_emergencyLog.clear();
    m_fallbackReason.clear();
    m_metric.lockWaitCount = 0;
    m_metric.totalLockWaitMicroseconds = 0.0;
    m_metric.avgLockWaitMicroseconds = 0.0;
    m_metric.status = ThreadStatus::IDLE;
}

void TrafficController::setConfig(const SimulationConfig& cfg) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_config = cfg;
    for (int i = 0; i < 4; ++i) {
        if (m_workers[i]) {
            m_workers[i]->setArrivalRatePerMin(cfg.arrivalRatePerMin[i]);
            m_workers[i]->setTimeSpeedMultiplier(cfg.timeSpeedMultiplier);
        }
    }
}

SimulationConfig TrafficController::getConfig() const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_config;
}

void TrafficController::requestEmergencyPreemption(Direction dir, const std::string& vehicleInfo) {
    auto now = std::chrono::steady_clock::now();
    double currentSec = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()) / 1000.0;

    m_emergencyDir.store(dir);
    m_emergencyRequestTimeSec = currentSec;
    m_emergencyActive.store(true);
    m_emergencyLog = "Emergency requested on " + std::string(directionToString(dir)) +
                     " (" + vehicleInfo + ") at T=" + std::to_string(currentSec) + "s";
}

void TrafficController::injectProcessingDelay(bool delayActive, int delayMs) {
    m_delayInjected.store(delayActive);
    m_injectedDelayMs.store(delayMs);
}

void TrafficController::enterSafeFallbackMode(const std::string& reason) {
    m_safeFallbackActive.store(true);
    m_fallbackReason = reason;
}

void TrafficController::exitSafeFallbackMode() {
    m_safeFallbackActive.store(false);
    m_fallbackReason.clear();
}

SignalColor TrafficController::getSignal(Direction dir) const {
    SignalPhase phase = m_currentPhase.load();
    switch (phase) {
        case SignalPhase::NORTH_SOUTH_GREEN:
            return (dir == Direction::NORTH || dir == Direction::SOUTH) ? SignalColor::GREEN : SignalColor::RED;
        case SignalPhase::NORTH_SOUTH_YELLOW:
            return (dir == Direction::NORTH || dir == Direction::SOUTH) ? SignalColor::YELLOW : SignalColor::RED;
        case SignalPhase::EAST_WEST_GREEN:
            return (dir == Direction::EAST || dir == Direction::WEST) ? SignalColor::GREEN : SignalColor::RED;
        case SignalPhase::EAST_WEST_YELLOW:
            return (dir == Direction::EAST || dir == Direction::WEST) ? SignalColor::YELLOW : SignalColor::RED;
        case SignalPhase::EMERGENCY_PREEMPTION: {
            Direction eDir = m_emergencyDir.load();
            if (eDir == Direction::NORTH || eDir == Direction::SOUTH) {
                return (dir == Direction::NORTH || dir == Direction::SOUTH) ? SignalColor::GREEN : SignalColor::RED;
            } else {
                return (dir == Direction::EAST || dir == Direction::WEST) ? SignalColor::GREEN : SignalColor::RED;
            }
        }
        case SignalPhase::ALL_RED_TO_EW:
        case SignalPhase::ALL_RED_TO_NS:
        case SignalPhase::SAFE_FALLBACK:
        default:
            return SignalColor::RED;
    }
}

void TrafficController::updateLaneWorkersSignals() {
    for (int i = 0; i < 4; ++i) {
        if (m_workers[i]) {
            m_workers[i]->setSignalColor(getSignal(static_cast<Direction>(i)));
        }
    }
}

void TrafficController::decideNextPhaseAndDuration(SignalPhase& outNextPhase, double& outDurationSec) {
    auto tStart = std::chrono::high_resolution_clock::now();
    m_metric.status = ThreadStatus::RUNNING;

    // Simulate artificial controller processing delay if injected
    if (m_delayInjected.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(m_injectedDelayMs.load()));
    }

    SimulationConfig cfg = getConfig();
    auto now = std::chrono::steady_clock::now();
    double currentSimSec = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()) / 1000.0;

    // Check emergency preemption first
    if (m_emergencyActive.load()) {
        Direction eDir = m_emergencyDir.load();
        if (eDir == Direction::NORTH || eDir == Direction::SOUTH) {
            outNextPhase = SignalPhase::NORTH_SOUTH_GREEN;
        } else {
            outNextPhase = SignalPhase::EAST_WEST_GREEN;
        }
        outDurationSec = 8.0; // Adequate time to clear emergency vehicle
        m_emergencyGrantTimeSec = currentSimSec;
        m_emergencyLog += " -> Granted at T=" + std::to_string(currentSimSec) + "s (Latency: " +
                          std::to_string((currentSimSec - m_emergencyRequestTimeSec) * 1000.0) + "ms)";
        auto tEnd = std::chrono::high_resolution_clock::now();
        m_decisionLatencyMicrosec.store(std::chrono::duration<double, std::micro>(tEnd - tStart).count());
        return;
    }

    // Safe fallback mode: round-robin fixed timing
    if (m_safeFallbackActive.load()) {
        SignalPhase cur = m_currentPhase.load();
        if (cur == SignalPhase::ALL_RED_TO_EW) {
            outNextPhase = SignalPhase::EAST_WEST_GREEN;
        } else {
            outNextPhase = SignalPhase::NORTH_SOUTH_GREEN;
        }
        outDurationSec = 8.0;
        auto tEnd = std::chrono::high_resolution_clock::now();
        m_decisionLatencyMicrosec.store(std::chrono::duration<double, std::micro>(tEnd - tStart).count());
        return;
    }

    // Baseline A: Fixed-Time Round-Robin
    if (cfg.algorithm == AlgorithmType::BASELINE_A_FIXED_TIME) {
        SignalPhase cur = m_currentPhase.load();
        if (cur == SignalPhase::ALL_RED_TO_EW) {
            outNextPhase = SignalPhase::EAST_WEST_GREEN;
        } else {
            outNextPhase = SignalPhase::NORTH_SOUTH_GREEN;
        }
        outDurationSec = cfg.fixedTimeGreenSec;
        auto tEnd = std::chrono::high_resolution_clock::now();
        m_decisionLatencyMicrosec.store(std::chrono::duration<double, std::micro>(tEnd - tStart).count());
        return;
    }

    // For Baseline B and Proposed C: Compute statistics
    LaneStats stats[4];
    if (cfg.syncStrategy == SyncStrategy::MESSAGE_QUEUE && m_msgQueue) {
        std::lock_guard<std::mutex> lk(m_mqStatsMutex);
        for (int i = 0; i < 4; ++i) stats[i] = m_cachedMqStats[i];
    } else {
        for (int i = 0; i < 4; ++i) {
            stats[i] = m_lanes[i]->computeStats(currentSimSec, cfg.starvationThresholdSec,
                                                cfg.alphaDensity, cfg.betaWaitTime, cfg.gammaStarvation,
                                                &m_metric);
        }
    }

    // Check starvation threshold violations
    for (int i = 0; i < 4; ++i) {
        if (stats[i].maxWaitTimeSec > cfg.starvationThresholdSec) {
            m_starvationCount++;
        }
    }

    double scoreNS = 0.0;
    double scoreEW = 0.0;

    if (cfg.algorithm == AlgorithmType::BASELINE_B_ADAPTIVE) {
        // Density only: alpha = 1.0, beta = 0.0, gamma = 0.0
        scoreNS = static_cast<double>(stats[0].queueLength + stats[1].queueLength);
        scoreEW = static_cast<double>(stats[2].queueLength + stats[3].queueLength);
    } else {
        // Proposed C: alpha, beta, gamma formula
        scoreNS = stats[0].starvationScore + stats[1].starvationScore;
        scoreEW = stats[2].starvationScore + stats[3].starvationScore;
    }

    // Enforce alternation if both queues are roughly balanced or minimum service requirements
    SignalPhase cur = m_currentPhase.load();
    if (cur == SignalPhase::ALL_RED_TO_EW) {
        outNextPhase = SignalPhase::EAST_WEST_GREEN;
    } else if (cur == SignalPhase::ALL_RED_TO_NS) {
        outNextPhase = SignalPhase::NORTH_SOUTH_GREEN;
    } else {
        // Choose phase with higher score
        if (scoreEW > scoreNS * 1.1) {
            outNextPhase = SignalPhase::EAST_WEST_GREEN;
        } else {
            outNextPhase = SignalPhase::NORTH_SOUTH_GREEN;
        }
    }

    // Calculate dynamic duration
    double totalScore = scoreNS + scoreEW;
    double activeScore = (outNextPhase == SignalPhase::NORTH_SOUTH_GREEN) ? scoreNS : scoreEW;

    if (totalScore <= 0.001) {
        outDurationSec = cfg.minGreenSec;
    } else {
        double ratio = activeScore / totalScore;
        outDurationSec = cfg.minGreenSec + (cfg.maxGreenSec - cfg.minGreenSec) * ratio;
        outDurationSec = std::clamp(outDurationSec, cfg.minGreenSec, cfg.maxGreenSec);
    }

    auto tEnd = std::chrono::high_resolution_clock::now();
    double latency = std::chrono::duration<double, std::micro>(tEnd - tStart).count();
    m_decisionLatencyMicrosec.store(latency);

    // Deadline check (50ms = 50,000us)
    if (latency > 50000.0) {
        m_deadlineMissCount++;
    }
}

void TrafficController::controllerLoop() {
#ifdef _WIN32
    m_metric.osThreadId = static_cast<uint32_t>(GetCurrentThreadId());
#endif

    m_metric.status = ThreadStatus::RUNNING;
    m_currentPhase.store(SignalPhase::NORTH_SOUTH_GREEN);
    
    auto startTime = std::chrono::steady_clock::now();
    double currentSec = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(startTime.time_since_epoch()).count()) / 1000.0;
    
    m_phaseStartTimeSec.store(currentSec);
    m_phaseDurationSec.store(10.0);
    updateLaneWorkersSignals();

    while (m_running.load()) {
        auto now = std::chrono::steady_clock::now();
        double currentSimSec = static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()) / 1000.0;

        // Drain MessageQueue if in MQ mode
        if (m_msgQueue) {
            LaneMessage msg;
            while (m_msgQueue->tryPop(msg)) {
                std::lock_guard<std::mutex> lk(m_mqStatsMutex);
                m_cachedMqStats[static_cast<int>(msg.direction)] = msg.stats;
            }
        }

        m_metric.lastHeartbeatSec = currentSimSec;
        SimulationConfig cfg = getConfig();
        double speed = (cfg.timeSpeedMultiplier > 0.05) ? cfg.timeSpeedMultiplier : 1.0;

        double elapsed = (currentSimSec - m_phaseStartTimeSec.load()) * speed;
        SignalPhase cur = m_currentPhase.load();

        switch (cur) {
            case SignalPhase::NORTH_SOUTH_GREEN: {
                // If emergency request is for EW, interrupt immediately to yellow
                if (m_emergencyActive.load() &&
                    (m_emergencyDir.load() == Direction::EAST || m_emergencyDir.load() == Direction::WEST)) {
                    m_currentPhase.store(SignalPhase::NORTH_SOUTH_YELLOW);
                    m_phaseStartTimeSec.store(currentSimSec);
                    m_phaseDurationSec.store(cfg.yellowSec);
                } else if (elapsed >= m_phaseDurationSec.load()) {
                    m_currentPhase.store(SignalPhase::NORTH_SOUTH_YELLOW);
                    m_phaseStartTimeSec.store(currentSimSec);
                    m_phaseDurationSec.store(cfg.yellowSec);
                }
                break;
            }
            case SignalPhase::NORTH_SOUTH_YELLOW: {
                if (elapsed >= cfg.yellowSec) {
                    m_currentPhase.store(SignalPhase::ALL_RED_TO_EW);
                    m_phaseStartTimeSec.store(currentSimSec);
                    m_phaseDurationSec.store(cfg.allRedSec);
                }
                break;
            }
            case SignalPhase::ALL_RED_TO_EW: {
                if (elapsed >= cfg.allRedSec) {
                    SignalPhase nextP;
                    double duration;
                    decideNextPhaseAndDuration(nextP, duration);
                    m_currentPhase.store(nextP);
                    m_phaseStartTimeSec.store(currentSimSec);
                    m_phaseDurationSec.store(duration);

                    // If emergency was satisfied, reset active flag after green begins
                    if (m_emergencyActive.load() &&
                        (m_emergencyDir.load() == Direction::EAST || m_emergencyDir.load() == Direction::WEST)) {
                        m_emergencyActive.store(false);
                    }
                }
                break;
            }
            case SignalPhase::EAST_WEST_GREEN: {
                // If emergency request is for NS, interrupt immediately to yellow
                if (m_emergencyActive.load() &&
                    (m_emergencyDir.load() == Direction::NORTH || m_emergencyDir.load() == Direction::SOUTH)) {
                    m_currentPhase.store(SignalPhase::EAST_WEST_YELLOW);
                    m_phaseStartTimeSec.store(currentSimSec);
                    m_phaseDurationSec.store(cfg.yellowSec);
                } else if (elapsed >= m_phaseDurationSec.load()) {
                    m_currentPhase.store(SignalPhase::EAST_WEST_YELLOW);
                    m_phaseStartTimeSec.store(currentSimSec);
                    m_phaseDurationSec.store(cfg.yellowSec);
                }
                break;
            }
            case SignalPhase::EAST_WEST_YELLOW: {
                if (elapsed >= cfg.yellowSec) {
                    m_currentPhase.store(SignalPhase::ALL_RED_TO_NS);
                    m_phaseStartTimeSec.store(currentSimSec);
                    m_phaseDurationSec.store(cfg.allRedSec);
                }
                break;
            }
            case SignalPhase::ALL_RED_TO_NS: {
                if (elapsed >= cfg.allRedSec) {
                    SignalPhase nextP;
                    double duration;
                    decideNextPhaseAndDuration(nextP, duration);
                    m_currentPhase.store(nextP);
                    m_phaseStartTimeSec.store(currentSimSec);
                    m_phaseDurationSec.store(duration);

                    if (m_emergencyActive.load() &&
                        (m_emergencyDir.load() == Direction::NORTH || m_emergencyDir.load() == Direction::SOUTH)) {
                        m_emergencyActive.store(false);
                    }
                }
                break;
            }
            default:
                m_currentPhase.store(SignalPhase::NORTH_SOUTH_GREEN);
                m_phaseStartTimeSec.store(currentSimSec);
                m_phaseDurationSec.store(10.0);
                break;
        }

        updateLaneWorkersSignals();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    m_metric.status = ThreadStatus::IDLE;
}

IntersectionSnapshot TrafficController::getSnapshot() {
    IntersectionSnapshot snap;
    auto now = std::chrono::steady_clock::now();
    double currentSimSec = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()) / 1000.0;

    snap.simTimeSec = currentSimSec;
    snap.isRunning = m_running.load();
    snap.isPaused = m_paused.load();
    
    SimulationConfig cfg = getConfig();
    snap.algorithm = cfg.algorithm;
    snap.syncStrategy = cfg.syncStrategy;
    snap.currentPhase = m_currentPhase.load();
    snap.phaseElapsedSec = (currentSimSec - m_phaseStartTimeSec.load()) * cfg.timeSpeedMultiplier;
    snap.currentPhaseDurationSec = m_phaseDurationSec.load();

    for (int i = 0; i < 4; ++i) {
        snap.signals[i] = getSignal(static_cast<Direction>(i));
        snap.lanes[i] = m_lanes[i]->computeStats(currentSimSec, cfg.starvationThresholdSec,
                                                cfg.alphaDensity, cfg.betaWaitTime, cfg.gammaStarvation);
    }

    // Safety constraint check (FR-01, FR-05): Check if conflicting greens ever occur!
    bool nsGreen = (snap.signals[0] == SignalColor::GREEN || snap.signals[1] == SignalColor::GREEN);
    bool ewGreen = (snap.signals[2] == SignalColor::GREEN || snap.signals[3] == SignalColor::GREEN);
    snap.safetyConflictDetected = (nsGreen && ewGreen);

    // Aggregate stats
    uint64_t totalServed = 0;
    double sumWait = 0.0;
    double maxWait = 0.0;
    for (int i = 0; i < 4; ++i) {
        totalServed += snap.lanes[i].totalVehiclesServed;
        sumWait += snap.lanes[i].avgWaitTimeSec * snap.lanes[i].queueLength;
        if (snap.lanes[i].maxWaitTimeSec > maxWait) {
            maxWait = snap.lanes[i].maxWaitTimeSec;
        }
    }

    snap.totalVehiclesProcessed = totalServed;
    int totalQueued = snap.lanes[0].queueLength + snap.lanes[1].queueLength +
                      snap.lanes[2].queueLength + snap.lanes[3].queueLength;
    snap.overallAvgWaitSec = (totalQueued > 0) ? (sumWait / totalQueued) : 0.0;
    snap.overallMaxWaitSec = maxWait;
    snap.starvationViolationsCount = m_starvationCount.load();

    // Jain's Fairness Index: (sum(xi))^2 / (n * sum(xi^2))
    double sumX = 0.0;
    double sumX2 = 0.0;
    for (int i = 0; i < 4; ++i) {
        double served = static_cast<double>(snap.lanes[i].totalVehiclesServed + 1); // laplace smoothing
        sumX += served;
        sumX2 += (served * served);
    }
    snap.jainsFairnessIndex = (sumX * sumX) / (4.0 * sumX2);

    snap.controllerDecisionLatencyMicrosec = m_decisionLatencyMicrosec.load();
    snap.deadlineMissCount = m_deadlineMissCount.load();
    snap.isSafeFallbackActive = m_safeFallbackActive.load();
    snap.lastEmergencyLog = m_emergencyLog;

    // Collect thread metrics
    snap.threads.push_back(m_metric);
    for (int i = 0; i < 4; ++i) {
        if (m_workers[i]) {
            snap.threads.push_back(m_workers[i]->getMetric());
        }
    }

    // Active vehicles snapshot for visual dashboard
    for (int i = 0; i < 4; ++i) {
        auto laneVehicles = m_lanes[i]->getVehicleSnapshot(10);
        snap.activeVehicles.insert(snap.activeVehicles.end(), laneVehicles.begin(), laneVehicles.end());
    }

    return snap;
}

} // namespace TrafficSim
