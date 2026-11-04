#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <cstdint>

namespace TrafficSim {

enum class Direction : int {
    NORTH = 0,
    SOUTH = 1,
    EAST = 2,
    WEST = 3,
    TOTAL_DIRECTIONS = 4
};

inline const char* directionToString(Direction dir) {
    switch (dir) {
        case Direction::NORTH: return "NORTH";
        case Direction::SOUTH: return "SOUTH";
        case Direction::EAST:  return "EAST";
        case Direction::WEST:  return "WEST";
        default: return "UNKNOWN";
    }
}

enum class SignalColor : int {
    RED = 0,
    YELLOW = 1,
    GREEN = 2
};

inline const char* signalColorToString(SignalColor color) {
    switch (color) {
        case SignalColor::RED:    return "RED";
        case SignalColor::YELLOW: return "YELLOW";
        case SignalColor::GREEN:  return "GREEN";
        default: return "UNKNOWN";
    }
}

enum class SignalPhase : int {
    NORTH_SOUTH_GREEN = 0,
    NORTH_SOUTH_YELLOW = 1,
    ALL_RED_TO_EW = 2,
    EAST_WEST_GREEN = 3,
    EAST_WEST_YELLOW = 4,
    ALL_RED_TO_NS = 5,
    EMERGENCY_PREEMPTION = 6,
    SAFE_FALLBACK = 7
};

inline const char* phaseToString(SignalPhase phase) {
    switch (phase) {
        case SignalPhase::NORTH_SOUTH_GREEN:  return "NS_GREEN";
        case SignalPhase::NORTH_SOUTH_YELLOW: return "NS_YELLOW";
        case SignalPhase::ALL_RED_TO_EW:      return "ALL_RED_TO_EW";
        case SignalPhase::EAST_WEST_GREEN:    return "EW_GREEN";
        case SignalPhase::EAST_WEST_YELLOW:   return "EW_YELLOW";
        case SignalPhase::ALL_RED_TO_NS:      return "ALL_RED_TO_NS";
        case SignalPhase::EMERGENCY_PREEMPTION: return "EMERGENCY";
        case SignalPhase::SAFE_FALLBACK:      return "SAFE_FALLBACK";
        default: return "UNKNOWN";
    }
}

enum class ThreadStatus : int {
    IDLE = 0,
    RUNNING = 1,
    WAITING_FOR_LOCK = 2,
    WAITING_FOR_EVENT = 3,
    FAILED = 4,
    RECOVERING = 5
};

inline const char* threadStatusToString(ThreadStatus status) {
    switch (status) {
        case ThreadStatus::IDLE:              return "IDLE";
        case ThreadStatus::RUNNING:           return "RUNNING";
        case ThreadStatus::WAITING_FOR_LOCK:  return "WAITING_LOCK";
        case ThreadStatus::WAITING_FOR_EVENT: return "WAITING_EVENT";
        case ThreadStatus::FAILED:            return "FAILED";
        case ThreadStatus::RECOVERING:        return "RECOVERING";
        default: return "UNKNOWN";
    }
}

enum class AlgorithmType : int {
    BASELINE_A_FIXED_TIME = 0,
    BASELINE_B_ADAPTIVE = 1,
    PROPOSED_C_FAIRNESS_FAULT_TOLERANT = 2
};

inline const char* algorithmToString(AlgorithmType alg) {
    switch (alg) {
        case AlgorithmType::BASELINE_A_FIXED_TIME:
            return "Baseline A: Fixed-Time";
        case AlgorithmType::BASELINE_B_ADAPTIVE:
            return "Baseline B: Adaptive (Density-Only)";
        case AlgorithmType::PROPOSED_C_FAIRNESS_FAULT_TOLERANT:
            return "Proposed C: Starvation-Resistant & Fault-Tolerant";
        default:
            return "Unknown";
    }
}

enum class SyncStrategy : int {
    SHARED_MUTEX = 0,
    MESSAGE_QUEUE = 1
};

inline const char* syncStrategyToString(SyncStrategy strat) {
    switch (strat) {
        case SyncStrategy::SHARED_MUTEX:  return "Shared-Memory Mutex";
        case SyncStrategy::MESSAGE_QUEUE: return "Message-Queue (Channel)";
        default: return "Unknown";
    }
}

enum class FaultType : int {
    NONE = 0,
    WORKER_HANG = 1,
    CONTROLLER_DELAY = 2,
    STALE_UPDATES = 3
};

inline const char* faultTypeToString(FaultType f) {
    switch (f) {
        case FaultType::NONE:             return "None";
        case FaultType::WORKER_HANG:      return "Worker Deadlock/Hang";
        case FaultType::CONTROLLER_DELAY: return "Controller Processing Lag";
        case FaultType::STALE_UPDATES:    return "Stale Telemetry Data";
        default: return "Unknown";
    }
}

enum class VehicleType : int {
    CAR = 0,
    BUS = 1,
    TRUCK = 2,
    AMBULANCE = 3,
    FIRE_TRUCK = 4
};

inline const char* vehicleTypeToString(VehicleType type) {
    switch (type) {
        case VehicleType::CAR:        return "Car";
        case VehicleType::BUS:        return "Bus";
        case VehicleType::TRUCK:      return "Truck";
        case VehicleType::AMBULANCE:  return "Ambulance";
        case VehicleType::FIRE_TRUCK: return "Fire Truck";
        default: return "Car";
    }
}

struct Vehicle {
    uint64_t id{0};
    Direction direction{Direction::NORTH};
    VehicleType type{VehicleType::CAR};
    bool isEmergency{false};
    double arrivalTimeSec{0.0};
    double startServiceTimeSec{0.0};
    double exitTimeSec{0.0};
    double waitTimeSec{0.0};
    double distanceToStopLineM{50.0}; // in meters
    double speedMps{10.0};            // meters per second
    bool crossedIntersection{false};
};

struct ThreadMetric {
    std::string threadName;
    uint32_t osThreadId{0};
    ThreadStatus status{ThreadStatus::IDLE};
    double lastHeartbeatSec{0.0};
    uint64_t lockWaitCount{0};
    double totalLockWaitMicroseconds{0.0};
    double avgLockWaitMicroseconds{0.0};
    bool isHealthy{true};
};

struct LaneStats {
    int queueLength{0};
    double avgWaitTimeSec{0.0};
    double maxWaitTimeSec{0.0};
    uint64_t totalVehiclesServed{0};
    uint64_t totalVehiclesArrived{0};
    double starvationScore{0.0};
    bool hasEmergencyVehicle{false};
};

struct IntersectionSnapshot {
    double simTimeSec{0.0};
    bool isRunning{false};
    bool isPaused{false};
    AlgorithmType algorithm{AlgorithmType::PROPOSED_C_FAIRNESS_FAULT_TOLERANT};
    SyncStrategy syncStrategy{SyncStrategy::SHARED_MUTEX};
    SignalPhase currentPhase{SignalPhase::NORTH_SOUTH_GREEN};
    double phaseElapsedSec{0.0};
    double currentPhaseDurationSec{10.0};
    
    // Per-direction signals
    SignalColor signals[4]{SignalColor::RED, SignalColor::RED, SignalColor::RED, SignalColor::RED};
    
    // Lane stats
    LaneStats lanes[4];
    
    // Aggregate traffic metrics
    uint64_t totalVehiclesProcessed{0};
    double throughputVehiclesPerMin{0.0};
    double overallAvgWaitSec{0.0};
    double overallMaxWaitSec{0.0};
    uint64_t starvationViolationsCount{0};
    double jainsFairnessIndex{1.0};

    // OS execution metrics
    double cpuUtilizationPercent{0.0};
    double memoryUsageMb{0.0};
    double controllerDecisionLatencyMicrosec{0.0};
    double avgSyncOverheadMicrosec{0.0};
    uint64_t deadlineMissCount{0};

    // Thread telemetry
    std::vector<ThreadMetric> threads;

    // Safety & Fault telemetry
    bool safetyConflictDetected{false};
    bool isSafeFallbackActive{false};
    FaultType activeFault{FaultType::NONE};
    int faultTargetWorkerIndex{-1};
    std::string lastFaultLog;
    std::string lastEmergencyLog;

    // Vehicles currently rendered near intersection
    std::vector<Vehicle> activeVehicles;
};

struct SimulationConfig {
    AlgorithmType algorithm{AlgorithmType::PROPOSED_C_FAIRNESS_FAULT_TOLERANT};
    SyncStrategy syncStrategy{SyncStrategy::SHARED_MUTEX};
    double arrivalRatePerMin[4]{20.0, 20.0, 20.0, 20.0}; // N, S, E, W
    double minGreenSec{5.0};
    double maxGreenSec{25.0};
    double yellowSec{2.5};
    double allRedSec{1.0};
    double fixedTimeGreenSec{10.0};
    double starvationThresholdSec{20.0};
    double alphaDensity{0.45};
    double betaWaitTime{0.35};
    double gammaStarvation{0.20};
    double timeSpeedMultiplier{1.0}; // 1.0 = real-time, 2.0 = 2x speed, etc.
};

struct BenchmarkResult {
    std::string testScenarioName;
    std::string algorithmName;
    std::string syncStrategyName;
    double durationSec{0.0};
    uint64_t totalServed{0};
    double throughputPerMin{0.0};
    double avgWaitSec{0.0};
    double maxWaitSec{0.0};
    double jainsFairnessIndex{0.0};
    uint64_t starvationEvents{0};
    double avgCpuPercent{0.0};
    double peakMemoryMb{0.0};
    double avgControllerLatencyMicrosec{0.0};
    double avgSyncOverheadMicrosec{0.0};
    uint64_t deadlineMisses{0};
};

} // namespace TrafficSim
