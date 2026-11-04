#pragma once

#include "Common.h"
#include <deque>
#include <mutex>
#include <chrono>

namespace TrafficSim {

class Lane {
public:
    explicit Lane(Direction dir);

    Direction getDirection() const { return m_direction; }

    // Enqueue a vehicle safely
    void addVehicle(const Vehicle& vehicle, ThreadMetric* workerMetric = nullptr);

    // Dequeue a vehicle that has passed the intersection
    bool removeVehicle(Vehicle* outVehicle, ThreadMetric* workerMetric = nullptr);

    // Get current queue length safely
    int getQueueLength();

    // Check if there is an emergency vehicle waiting
    bool hasEmergencyVehicle();

    // Compute stats (density, average waiting time, max waiting time, starvation score)
    LaneStats computeStats(double currentSimTime, double starvationThresholdSec,
                           double alpha, double beta, double gamma,
                           ThreadMetric* metric = nullptr);

    // Get a copy of waiting vehicles for rendering / analytics
    std::vector<Vehicle> getVehicleSnapshot(size_t maxCount = 20);

    // Reset lane data
    void reset();

    // Direct access to lock for custom instrumentation if needed
    std::mutex& getMutex() { return m_mutex; }

private:
    Direction m_direction;
    std::mutex m_mutex;
    std::deque<Vehicle> m_queue;
    uint64_t m_totalArrived{0};
    uint64_t m_totalServed{0};
    double m_accumulatedWaitTimeSec{0.0};
};

} // namespace TrafficSim
