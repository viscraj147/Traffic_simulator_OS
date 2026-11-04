#include "EmergencyHandler.h"
#include <chrono>

namespace TrafficSim {

EmergencyHandler::EmergencyHandler(TrafficController* controller, LaneWorker* workers[4])
    : m_controller(controller) {
    for (int i = 0; i < 4; ++i) {
        m_workers[i] = workers[i];
    }
}

EmergencyEvent EmergencyHandler::dispatchEmergency(Direction dir, VehicleType type) {
    auto now = std::chrono::steady_clock::now();
    double currentSec = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()) / 1000.0;

    EmergencyEvent evt;
    evt.id = m_nextId++;
    evt.direction = dir;
    evt.vehicleType = type;
    evt.requestTimeSec = currentSec;
    evt.grantTimeSec = 0.0;
    evt.responseDelayMs = 0.0;
    evt.isCompleted = false;

    // 1. Inject the vehicle into the chosen lane worker
    int idx = static_cast<int>(dir);
    if (m_workers[idx]) {
        m_workers[idx]->injectVehicle(type, true);
    }

    // 2. Request priority preemption from controller
    if (m_controller) {
        m_controller->requestEmergencyPreemption(dir, vehicleTypeToString(type));
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_history.push_back(evt);
    return evt;
}

std::vector<EmergencyEvent> EmergencyHandler::getHistory() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_history;
}

void EmergencyHandler::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_history.clear();
    m_nextId = 1;
}

} // namespace TrafficSim
