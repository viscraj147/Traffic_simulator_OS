#pragma once

#include "Common.h"
#include "TrafficController.h"
#include <mutex>
#include <vector>
#include <string>

namespace TrafficSim {

struct EmergencyEvent {
    uint64_t id;
    Direction direction;
    VehicleType vehicleType;
    double requestTimeSec;
    double grantTimeSec;
    double responseDelayMs;
    bool isCompleted;
};

class EmergencyHandler {
public:
    EmergencyHandler(TrafficController* controller, LaneWorker* workers[4]);

    // Trigger an emergency dispatch
    EmergencyEvent dispatchEmergency(Direction dir, VehicleType type = VehicleType::AMBULANCE);

    // Retrieve historical log of emergency preemption events
    std::vector<EmergencyEvent> getHistory();

    void reset();

private:
    TrafficController* m_controller;
    LaneWorker* m_workers[4];
    std::mutex m_mutex;
    std::vector<EmergencyEvent> m_history;
    uint64_t m_nextId{1};
};

} // namespace TrafficSim
