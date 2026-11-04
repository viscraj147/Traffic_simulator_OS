#pragma once

#include "Common.h"
#include "Lane.h"
#include "MessageQueue.h"
#include <thread>
#include <atomic>
#include <random>

namespace TrafficSim {

class TrafficController;

class LaneWorker {
public:
    LaneWorker(Direction dir, Lane* lane, MessageQueue<LaneMessage>* msgQueue = nullptr);
    ~LaneWorker();

    void start();
    void stop();
    void reset();

    // Fault injection controls
    void injectHang(bool hang);
    void injectStaleUpdates(bool stale);
    void recover();

    // Direct injection of emergency vehicle
    void injectVehicle(VehicleType type = VehicleType::CAR, bool isEmergency = false);

    Direction getDirection() const { return m_direction; }
    ThreadMetric& getMetric() { return m_metric; }
    bool isHanging() const { return m_hangInjected.load(); }
    bool isStale() const { return m_staleInjected.load(); }

    void setArrivalRatePerMin(double rate) { m_arrivalRatePerMin.store(rate); }
    void setTimeSpeedMultiplier(double mult) { m_speedMult.store(mult); }
    void setSignalColor(SignalColor color) { m_currentSignal.store(color); }

private:
    void workerLoop();

    Direction m_direction;
    Lane* m_lane;
    MessageQueue<LaneMessage>* m_msgQueue;
    
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_hangInjected{false};
    std::atomic<bool> m_staleInjected{false};
    std::atomic<double> m_arrivalRatePerMin{20.0};
    std::atomic<double> m_speedMult{1.0};
    std::atomic<SignalColor> m_currentSignal{SignalColor::RED};

    ThreadMetric m_metric;
    std::mt19937 m_rng;
    uint64_t m_nextVehicleId{1};
};

} // namespace TrafficSim
