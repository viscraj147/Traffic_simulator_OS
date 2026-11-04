#include "LaneWorker.h"
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

namespace TrafficSim {

LaneWorker::LaneWorker(Direction dir, Lane* lane, MessageQueue<LaneMessage>* msgQueue)
    : m_direction(dir), m_lane(lane), m_msgQueue(msgQueue), m_rng(1337 + static_cast<int>(dir)) {
    m_metric.threadName = std::string(directionToString(dir)) + "_LaneWorker";
    m_metric.status = ThreadStatus::IDLE;
    m_metric.isHealthy = true;
}

LaneWorker::~LaneWorker() {
    stop();
}

void LaneWorker::start() {
    if (m_running.load()) return;
    m_running.store(true);
    m_thread = std::thread(&LaneWorker::workerLoop, this);
}

void LaneWorker::stop() {
    if (!m_running.load()) return;
    m_running.store(false);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_metric.status = ThreadStatus::IDLE;
}

void LaneWorker::reset() {
    stop();
    m_hangInjected.store(false);
    m_staleInjected.store(false);
    m_nextVehicleId = 1;
    m_metric.lockWaitCount = 0;
    m_metric.totalLockWaitMicroseconds = 0.0;
    m_metric.avgLockWaitMicroseconds = 0.0;
    m_metric.isHealthy = true;
    m_metric.status = ThreadStatus::IDLE;
    if (m_lane) {
        m_lane->reset();
    }
}

void LaneWorker::injectHang(bool hang) {
    m_hangInjected.store(hang);
    if (hang) {
        m_metric.status = ThreadStatus::FAILED;
        m_metric.isHealthy = false;
    }
}

void LaneWorker::injectStaleUpdates(bool stale) {
    m_staleInjected.store(stale);
}

void LaneWorker::recover() {
    m_hangInjected.store(false);
    m_staleInjected.store(false);
    m_metric.status = ThreadStatus::RECOVERING;
    m_metric.isHealthy = true;
}

void LaneWorker::injectVehicle(VehicleType type, bool isEmergency) {
    Vehicle v;
    v.id = (static_cast<uint64_t>(m_direction) << 32) | (m_nextVehicleId++);
    v.direction = m_direction;
    v.type = type;
    v.isEmergency = isEmergency;
    v.arrivalTimeSec = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
    v.speedMps = isEmergency ? 16.0 : 10.0;
    v.distanceToStopLineM = 60.0;
    v.crossedIntersection = false;

    if (m_lane) {
        m_lane->addVehicle(v, &m_metric);
    }
}

void LaneWorker::workerLoop() {
#ifdef _WIN32
    m_metric.osThreadId = static_cast<uint32_t>(GetCurrentThreadId());
#endif

    m_metric.status = ThreadStatus::RUNNING;
    auto lastArrivalCheck = std::chrono::steady_clock::now();
    auto lastDepartureCheck = std::chrono::steady_clock::now();
    auto lastHeartbeat = std::chrono::steady_clock::now();

    // Exponential distribution parameter: lambda = rate / 60.0
    while (m_running.load()) {
        auto now = std::chrono::steady_clock::now();
        double currentWallSec = static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()) / 1000.0;

        // If simulated hang is active, do NOT send heartbeats, simulate lock hang or freeze
        if (m_hangInjected.load()) {
            m_metric.status = ThreadStatus::FAILED;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (m_metric.status == ThreadStatus::RECOVERING) {
            m_metric.status = ThreadStatus::RUNNING;
        }

        // Emit heartbeat every 100ms
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHeartbeat).count() >= 100) {
            m_metric.lastHeartbeatSec = currentWallSec;
            m_metric.isHealthy = true;
            lastHeartbeat = now;
        }

        double speed = m_speedMult.load();
        if (speed <= 0.05) speed = 0.05;

        // 1. Arrival generation
        double ratePerMin = m_arrivalRatePerMin.load();
        if (ratePerMin > 0.0) {
            double meanIntervalSec = 60.0 / ratePerMin / speed;
            double elapsedArrival = std::chrono::duration<double>(now - lastArrivalCheck).count();

            // Check if vehicle arrival condition met
            std::exponential_distribution<double> expDist(1.0 / meanIntervalSec);
            double sample = expDist(m_rng);
            if (elapsedArrival >= sample) {
                lastArrivalCheck = now;
                // Randomly generate car, bus, or truck
                std::uniform_int_distribution<int> typeDist(0, 9);
                int r = typeDist(m_rng);
                VehicleType vt = VehicleType::CAR;
                if (r == 8) vt = VehicleType::BUS;
                else if (r == 9) vt = VehicleType::TRUCK;

                injectVehicle(vt, false);
            }
        }

        // 2. Vehicle departure when GREEN
        if (m_currentSignal.load() == SignalColor::GREEN && m_lane) {
            double departureInterval = (1.5 / speed); // 1 vehicle passes every ~1.5s real/scaled time
            double elapsedDep = std::chrono::duration<double>(now - lastDepartureCheck).count();
            if (elapsedDep >= departureInterval) {
                lastDepartureCheck = now;
                Vehicle v;
                if (m_lane->removeVehicle(&v, &m_metric)) {
                    v.exitTimeSec = currentWallSec;
                    v.crossedIntersection = true;
                }
            }
        } else {
            lastDepartureCheck = now;
        }

        // 3. Message Queue Mode: push lane state
        if (m_msgQueue && !m_staleInjected.load()) {
            LaneStats stats = m_lane->computeStats(currentWallSec, 20.0, 0.45, 0.35, 0.20, &m_metric);
            LaneMessage msg{m_direction, stats, currentWallSec, false};
            m_msgQueue->push(msg, &m_metric);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    m_metric.status = ThreadStatus::IDLE;
}

} // namespace TrafficSim
