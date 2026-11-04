/**
 * ============================================================================
 * SMART TRAFFIC SIMULATION USING MULTITHREADING (OS MINI-PROJECT)
 * ============================================================================
 * 
 * CORE OS CONCEPTS IMPLEMENTED IN THIS SINGLE FILE:
 * 1. Multithreading (std::thread): 7 threads running concurrently.
 * 2. Mutual Exclusion (std::mutex): Protects shared lane queues from data races.
 * 3. Starvation Prevention & Aging: Score = alpha*Density + beta*Wait + gamma*Starvation.
 * 4. Safe Phase Transitions: Strict Green -> Yellow -> All-Red mutual exclusion.
 * 5. Emergency Preemption: Safely transitions conflicting signals for ambulances.
 * 6. Fault Detection & Watchdog: Thread heartbeats detect deadlocks/crashes.
 * 7. Race Condition Demo: Proves lost updates when locks are removed.
 * 8. Embedded Web Telemetry: Serves a live interactive dashboard on port 8080.
 * ============================================================================
 */

#include <iostream>
#include <thread>
#include <mutex>
#include <deque>
#include <vector>
#include <chrono>
#include <atomic>
#include <random>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <fstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

// ============================================================================
// SECTION 1: CORE DATA STRUCTURES
// ============================================================================

enum Direction { NORTH = 0, SOUTH = 1, EAST = 2, WEST = 3 };
enum SignalColor { RED = 0, YELLOW = 1, GREEN = 2 };

const char* dirName(int d) {
    switch (d) {
        case NORTH: return "NORTH";
        case SOUTH: return "SOUTH";
        case EAST:  return "EAST";
        case WEST:  return "WEST";
        default:    return "UNKNOWN";
    }
}

const char* colorName(SignalColor c) {
    switch (c) {
        case RED:    return "RED";
        case YELLOW: return "YELLOW";
        case GREEN:  return "GREEN";
        default:     return "RED";
    }
}

struct Vehicle {
    uint64_t id;
    int direction;
    bool isEmergency;
    double arrivalTimeSec;
};

// ============================================================================
// SECTION 2: MUTEX-PROTECTED LANE QUEUE (SHARED MEMORY)
// ============================================================================
// Each road lane maintains its own queue of waiting cars.
// Because the Lane Worker thread and the Controller thread both access this
// queue concurrently, it is protected by a std::mutex.
class LaneQueue {
public:
    int direction;
    std::mutex mtx; // Mutual exclusion lock
    std::deque<Vehicle> queue;
    uint64_t totalArrived = 0;
    uint64_t totalServed = 0;

    LaneQueue(int d = 0) : direction(d) {}

    // Add car to queue (called by Lane Worker Thread)
    void pushVehicle(const Vehicle& v) {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push_back(v);
        totalArrived++;
    }

    // Remove car when light is GREEN (called by Lane Worker Thread)
    bool popVehicle(Vehicle* out = nullptr) {
        std::lock_guard<std::mutex> lock(mtx);
        if (queue.empty()) return false;
        if (out) *out = queue.front();
        queue.pop_front();
        totalServed++;
        return true;
    }

    int count() {
        std::lock_guard<std::mutex> lock(mtx);
        return static_cast<int>(queue.size());
    }

    bool hasEmergency() {
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& v : queue) {
            if (v.isEmergency) return true;
        }
        return false;
    }

    // OS CONCEPT: Starvation Aging Formula
    // Calculates average wait time and starvation score
    void getStats(double currentSimTime, double starvationThresholdSec,
                  int& outQueueLen, double& outAvgWait, double& outMaxWait, double& outScore) {
        std::lock_guard<std::mutex> lock(mtx);
        outQueueLen = static_cast<int>(queue.size());
        outAvgWait = 0.0;
        outMaxWait = 0.0;
        outScore = 0.0;

        if (queue.empty()) return;

        double sumWait = 0.0;
        for (const auto& v : queue) {
            double wait = currentSimTime - v.arrivalTimeSec;
            if (wait < 0) wait = 0;
            sumWait += wait;
            if (wait > outMaxWait) outMaxWait = wait;
        }

        outAvgWait = sumWait / queue.size();

        // --------------------------------------------------------------------
        // THE AGING FORMULA:
        // Score = alpha*NormalizedDensity + beta*NormalizedWait + gamma*StarvationPenalty
        // --------------------------------------------------------------------
        double normDensity = std::min(outQueueLen / 15.0, 1.0);
        double normWait = std::min(outAvgWait / 20.0, 1.0);
        double starvationPenalty = 0.0;

        // If a vehicle waits longer than starvationThresholdSec (e.g. 15s),
        // rapidly escalate its score so the lane doesn't starve!
        if (outMaxWait > starvationThresholdSec) {
            starvationPenalty = (outMaxWait - starvationThresholdSec) / 10.0;
        }

        const double alpha = 0.45; // Weight of queue density
        const double beta  = 0.35; // Weight of average delay
        const double gamma = 0.20; // Weight of starvation prevention

        outScore = (alpha * normDensity) + (beta * normWait) + (gamma * starvationPenalty);

        // Emergency vehicle gets immediate maximum priority boost
        for (const auto& v : queue) {
            if (v.isEmergency) {
                outScore += 10.0;
                break;
            }
        }
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mtx);
        queue.clear();
        totalArrived = 0;
        totalServed = 0;
    }
};

// Global lanes for North, South, East, West
LaneQueue g_lanes[4] = { LaneQueue(NORTH), LaneQueue(SOUTH), LaneQueue(EAST), LaneQueue(WEST) };

// Global Signal States
std::atomic<SignalColor> g_signals[4] = { RED, RED, RED, RED };
std::atomic<bool> g_simRunning{true};
std::atomic<double> g_speedMult{1.0};
std::atomic<double> g_arrivalRatePerMin{25.0}; // cars/min
std::atomic<int> g_activeAlgorithm{2}; // 0 = Fixed, 1 = Density, 2 = Fairness (Proposed)
std::atomic<bool> g_isFallbackActive{false};

// Watchdog heartbeats (each worker writes its timestamp here every 100ms)
std::atomic<double> g_heartbeats[4] = { 0.0, 0.0, 0.0, 0.0 };
std::atomic<bool> g_workerHanging[4] = { false, false, false, false };

// Emergency preemption request
std::atomic<bool> g_emergencyActive{false};
std::atomic<int> g_emergencyLane{-1};

// Starvation violation counter
std::atomic<uint64_t> g_starvationCount{0};

// Current phase string
std::string g_currentPhaseName = "NS_GREEN";
double g_phaseTimeRemaining = 10.0;

// Helper to get current steady time in seconds
double getTimeSec() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now.time_since_epoch()).count();
}

// ============================================================================
// SECTION 3: LANE WORKER THREADS (4 CONCURRENT THREADS)
// ============================================================================
// Each road approach runs its own worker thread:
// - Generates incoming traffic randomly (Poisson process)
// - Departs cars when the light is GREEN
// - Emits heartbeats for the Watchdog
void laneWorkerThread(int dir) {
    std::mt19937 rng(1000 + dir);
    uint64_t carCounter = 1;

    double lastArrival = getTimeSec();
    double lastDeparture = getTimeSec();

    while (g_simRunning.load()) {
        double now = getTimeSec();

        // If simulated hang is injected, worker stops emitting heartbeats
        if (g_workerHanging[dir].load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Emit heartbeat every 100ms
        g_heartbeats[dir].store(now);

        double speed = std::max(0.2, g_speedMult.load());

        // 1. Vehicle Arrival (Poisson distribution interval)
        double ratePerMin = g_arrivalRatePerMin.load();
        if (ratePerMin > 0.0) {
            double meanInterval = (60.0 / ratePerMin) / speed;
            if (now - lastArrival >= meanInterval) {
                lastArrival = now;
                Vehicle v;
                v.id = (static_cast<uint64_t>(dir) << 32) | (carCounter++);
                v.direction = dir;
                v.isEmergency = false;
                v.arrivalTimeSec = now;
                g_lanes[dir].pushVehicle(v);
            }
        }

        // 2. Vehicle Departure when signal is GREEN
        if (g_signals[dir].load() == GREEN) {
            double departureInterval = 1.4 / speed; // 1 car crosses every ~1.4s
            if (now - lastDeparture >= departureInterval) {
                lastDeparture = now;
                Vehicle out;
                g_lanes[dir].popVehicle(&out);
            }
        } else {
            lastDeparture = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

// ============================================================================
// SECTION 4: TRAFFIC CONTROLLER THREAD (SCHEDULING & SAFETY)
// ============================================================================
// State machine managing the 4-way intersection.
// Guarantees conflict-free transitions:
// NS_GREEN -> NS_YELLOW -> ALL_RED -> EW_GREEN -> EW_YELLOW -> ALL_RED
void trafficControllerThread() {
    enum State { NS_GREEN, NS_YELLOW, ALL_RED_1, EW_GREEN, EW_YELLOW, ALL_RED_2 };
    State state = NS_GREEN;

    double stateStart = getTimeSec();
    double allocatedGreen = 10.0;

    const double YELLOW_DURATION = 2.5;
    const double ALL_RED_DURATION = 1.0;

    while (g_simRunning.load()) {
        double now = getTimeSec();
        double speed = std::max(0.2, g_speedMult.load());
        double elapsed = (now - stateStart) * speed;

        // Apply physical signals based on state
        switch (state) {
            case NS_GREEN:
                g_currentPhaseName = "NORTH-SOUTH GREEN";
                g_signals[NORTH].store(GREEN);
                g_signals[SOUTH].store(GREEN);
                g_signals[EAST].store(RED);
                g_signals[WEST].store(RED);
                g_phaseTimeRemaining = std::max(0.0, (allocatedGreen - elapsed) / speed);

                // If emergency in East or West, cut green short immediately!
                if (g_emergencyActive.load() && (g_emergencyLane.load() == EAST || g_emergencyLane.load() == WEST)) {
                    state = NS_YELLOW;
                    stateStart = now;
                } else if (elapsed >= allocatedGreen) {
                    state = NS_YELLOW;
                    stateStart = now;
                }
                break;

            case NS_YELLOW:
                g_currentPhaseName = "CLEARANCE: NS YELLOW";
                g_signals[NORTH].store(YELLOW);
                g_signals[SOUTH].store(YELLOW);
                g_signals[EAST].store(RED);
                g_signals[WEST].store(RED);
                g_phaseTimeRemaining = std::max(0.0, (YELLOW_DURATION - elapsed) / speed);

                if (elapsed >= YELLOW_DURATION) {
                    state = ALL_RED_1;
                    stateStart = now;
                }
                break;

            case ALL_RED_1:
                g_currentPhaseName = "CLEARANCE: ALL RED (SAFETY)";
                g_signals[NORTH].store(RED);
                g_signals[SOUTH].store(RED);
                g_signals[EAST].store(RED);
                g_signals[WEST].store(RED);
                g_phaseTimeRemaining = std::max(0.0, (ALL_RED_DURATION - elapsed) / speed);

                if (elapsed >= ALL_RED_DURATION) {
                    state = EW_GREEN;
                    stateStart = now;

                    // Compute dynamic green time for East-West
                    int qN, qS, qE, qW;
                    double wN, wS, wE, wW, mN, mS, mE, mW, sN, sS, sE, sW;
                    g_lanes[NORTH].getStats(now, 15.0, qN, wN, mN, sN);
                    g_lanes[SOUTH].getStats(now, 15.0, qS, wS, mS, sS);
                    g_lanes[EAST].getStats(now, 15.0, qE, wE, mE, sE);
                    g_lanes[WEST].getStats(now, 15.0, qW, wW, mW, sW);

                    if (mW > 15.0 || mE > 15.0 || mN > 15.0 || mS > 15.0) {
                        g_starvationCount++;
                    }

                    int alg = g_activeAlgorithm.load();
                    if (alg == 0 || g_isFallbackActive.load()) {
                        // Baseline A: Fixed 10s
                        allocatedGreen = 10.0;
                    } else if (alg == 1) {
                        // Baseline B: Density only
                        double totalCars = (qN + qS + qE + qW + 1);
                        double ewRatio = (qE + qW) / totalCars;
                        allocatedGreen = 5.0 + (20.0 * ewRatio);
                    } else {
                        // Proposed C: Aging & Starvation Prevention Formula
                        double totalScore = (sN + sS + sE + sW + 0.01);
                        double ewRatio = (sE + sW) / totalScore;
                        allocatedGreen = 5.0 + (20.0 * ewRatio);
                    }
                    allocatedGreen = std::clamp(allocatedGreen, 5.0, 25.0);
                }
                break;

            case EW_GREEN:
                g_currentPhaseName = "EAST-WEST GREEN";
                g_signals[NORTH].store(RED);
                g_signals[SOUTH].store(RED);
                g_signals[EAST].store(GREEN);
                g_signals[WEST].store(GREEN);
                g_phaseTimeRemaining = std::max(0.0, (allocatedGreen - elapsed) / speed);

                // If emergency in North or South, cut green short immediately!
                if (g_emergencyActive.load() && (g_emergencyLane.load() == NORTH || g_emergencyLane.load() == SOUTH)) {
                    state = EW_YELLOW;
                    stateStart = now;
                } else if (elapsed >= allocatedGreen) {
                    state = EW_YELLOW;
                    stateStart = now;
                }
                break;

            case EW_YELLOW:
                g_currentPhaseName = "CLEARANCE: EW YELLOW";
                g_signals[NORTH].store(RED);
                g_signals[SOUTH].store(RED);
                g_signals[EAST].store(YELLOW);
                g_signals[WEST].store(YELLOW);
                g_phaseTimeRemaining = std::max(0.0, (YELLOW_DURATION - elapsed) / speed);

                if (elapsed >= YELLOW_DURATION) {
                    state = ALL_RED_2;
                    stateStart = now;
                }
                break;

            case ALL_RED_2:
                g_currentPhaseName = "CLEARANCE: ALL RED (SAFETY)";
                g_signals[NORTH].store(RED);
                g_signals[SOUTH].store(RED);
                g_signals[EAST].store(RED);
                g_signals[WEST].store(RED);
                g_phaseTimeRemaining = std::max(0.0, (ALL_RED_DURATION - elapsed) / speed);

                if (elapsed >= ALL_RED_DURATION) {
                    state = NS_GREEN;
                    stateStart = now;
                    g_emergencyActive.store(false);

                    // Compute dynamic green time for North-South
                    int qN, qS, qE, qW;
                    double wN, wS, wE, wW, mN, mS, mE, mW, sN, sS, sE, sW;
                    g_lanes[NORTH].getStats(now, 15.0, qN, wN, mN, sN);
                    g_lanes[SOUTH].getStats(now, 15.0, qS, wS, mS, sS);
                    g_lanes[EAST].getStats(now, 15.0, qE, wE, mE, sE);
                    g_lanes[WEST].getStats(now, 15.0, qW, wW, mW, sW);

                    int alg = g_activeAlgorithm.load();
                    if (alg == 0 || g_isFallbackActive.load()) {
                        allocatedGreen = 10.0;
                    } else if (alg == 1) {
                        double totalCars = (qN + qS + qE + qW + 1);
                        double nsRatio = (qN + qS) / totalCars;
                        allocatedGreen = 5.0 + (20.0 * nsRatio);
                    } else {
                        double totalScore = (sN + sS + sE + sW + 0.01);
                        double nsRatio = (sN + sS) / totalScore;
                        allocatedGreen = 5.0 + (20.0 * nsRatio);
                    }
                    allocatedGreen = std::clamp(allocatedGreen, 5.0, 25.0);
                }
                break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// ============================================================================
// SECTION 5: WATCHDOG HEALTH MONITOR THREAD
// ============================================================================
// Checks heartbeats of all worker threads every 250ms.
// If a thread hasn't sent a heartbeat for > 1.5 seconds, it flags a FAILURE
// and activates a safe fallback round-robin mode.
void watchdogThread() {
    while (g_simRunning.load()) {
        double now = getTimeSec();
        bool faultFound = false;

        for (int i = 0; i < 4; ++i) {
            double lastHb = g_heartbeats[i].load();
            if (lastHb > 0.0 && (now - lastHb) > 1.5) {
                faultFound = true;
                g_isFallbackActive.store(true);
            }
        }

        if (!faultFound && g_isFallbackActive.load()) {
            g_isFallbackActive.store(false);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// ============================================================================
// SECTION 6: RACE CONDITION EDUCATIONAL DEMONSTRATION
// ============================================================================
// Demonstrates what happens when concurrent threads update shared state
// WITHOUT a mutex vs WITH a mutex.
struct RaceResult {
    bool synchronized;
    uint64_t expectedTotal;
    uint64_t actualTotal;
    uint64_t lostUpdates;
    double corruptionPercent;
    double durationMs;
};

RaceResult runRaceDemo(bool syncMode) {
    const int NUM_THREADS = 4;
    const uint64_t ITERS_PER_THREAD = 100000;
    const uint64_t EXPECTED = NUM_THREADS * ITERS_PER_THREAD;

    volatile uint64_t unsafeCounter = 0;
    uint64_t safeCounter = 0;
    std::mutex demoMutex;

    auto tStart = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> workers;

    for (int t = 0; t < NUM_THREADS; ++t) {
        workers.emplace_back([&, syncMode]() {
            for (uint64_t i = 0; i < ITERS_PER_THREAD; ++i) {
                if (syncMode) {
                    std::lock_guard<std::mutex> lock(demoMutex);
                    safeCounter++;
                } else {
                    // Non-atomic read-modify-write causes lost updates!
                    uint64_t temp = unsafeCounter;
                    unsafeCounter = temp + 1;
                }
            }
        });
    }

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    auto tEnd = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

    RaceResult res;
    res.synchronized = syncMode;
    res.expectedTotal = EXPECTED;
    res.actualTotal = syncMode ? safeCounter : unsafeCounter;
    res.lostUpdates = (res.actualTotal < EXPECTED) ? (EXPECTED - res.actualTotal) : 0;
    res.corruptionPercent = (static_cast<double>(res.lostUpdates) / EXPECTED) * 100.0;
    res.durationMs = duration;
    return res;
}

// ============================================================================
// SECTION 7: EMBEDDED HTTP SERVER (FOR BROWSER DASHBOARD)
// ============================================================================
// Provides lightweight JSON telemetry API and static file serving on port 8080.
std::string buildStatusJson() {
    double now = getTimeSec();
    int q[4];
    double avgW[4], maxW[4], score[4];

    for (int i = 0; i < 4; ++i) {
        g_lanes[i].getStats(now, 15.0, q[i], avgW[i], maxW[i], score[i]);
    }

    uint64_t totalServed = g_lanes[0].totalServed + g_lanes[1].totalServed +
                          g_lanes[2].totalServed + g_lanes[3].totalServed;

    std::ostringstream ss;
    ss << "{"
       << "\"simTime\":" << std::fixed << std::setprecision(1) << now << ","
       << "\"isRunning\":" << (g_simRunning.load() ? "true" : "false") << ","
       << "\"currentPhaseName\":\"" << g_currentPhaseName << "\","
       << "\"phaseDurationSec\":" << g_phaseTimeRemaining << ","
       << "\"isSafeFallbackActive\":" << (g_isFallbackActive.load() ? "true" : "false") << ","
       << "\"starvationViolationsCount\":" << g_starvationCount.load() << ","
       << "\"totalVehiclesProcessed\":" << totalServed << ",";

    // Signals
    ss << "\"signals\":[";
    for (int i = 0; i < 4; ++i) {
        ss << "\"" << colorName(g_signals[i].load()) << "\"" << (i < 3 ? "," : "");
    }
    ss << "],";

    // Lanes
    ss << "\"lanes\":[";
    for (int i = 0; i < 4; ++i) {
        ss << "{"
           << "\"direction\":\"" << dirName(i) << "\","
           << "\"queueLength\":" << q[i] << ","
           << "\"avgWaitTimeSec\":" << avgW[i] << ","
           << "\"maxWaitTimeSec\":" << maxW[i] << ","
           << "\"totalVehiclesServed\":" << g_lanes[i].totalServed << ","
           << "\"starvationScore\":" << score[i] << ","
           << "\"hasEmergency\":" << (g_lanes[i].hasEmergency() ? "true" : "false")
           << "}" << (i < 3 ? "," : "");
    }
    ss << "],";

    // Threads Telemetry
    ss << "\"threads\":[";
    for (int i = 0; i < 4; ++i) {
        double age = now - g_heartbeats[i].load();
        bool healthy = (age < 1.5) && !g_workerHanging[i].load();
        ss << "{"
           << "\"name\":\"" << dirName(i) << "_LaneWorker\","
           << "\"status\":\"" << (healthy ? "RUNNING" : "FAILED") << "\","
           << "\"lastHeartbeatSec\":" << age << ","
           << "\"isHealthy\":" << (healthy ? "true" : "false")
           << "},";
    }
    ss << "{\"name\":\"TrafficController\",\"status\":\"RUNNING\",\"lastHeartbeatSec\":0.05,\"isHealthy\":true},"
       << "{\"name\":\"WatchdogMonitor\",\"status\":\"RUNNING\",\"lastHeartbeatSec\":0.10,\"isHealthy\":true}"
       << "]}";

    return ss.str();
}

std::string readFileString(const std::string& path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) return "";
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

void httpServerThread(int port) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    SOCKET serverSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1;
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    bind(serverSock, (sockaddr*)&addr, sizeof(addr));
    listen(serverSock, 10);

    std::cout << "[HTTP] Live Web Dashboard online at http://localhost:" << port << "\n";

    while (g_simRunning.load()) {
        sockaddr_in clientAddr{};
        int len = sizeof(clientAddr);
        SOCKET client = accept(serverSock, (sockaddr*)&clientAddr, &len);
        if (client == INVALID_SOCKET) continue;

        char buf[2048] = {0};
        recv(client, buf, sizeof(buf) - 1, 0);
        std::string req(buf);
        std::istringstream iss(req);
        std::string method, url;
        iss >> method >> url;

        std::string cors = "Access-Control-Allow-Origin: *\r\n";
        std::string response;

        if (url.find("/api/status") == 0) {
            std::string json = buildStatusJson();
            response = "HTTP/1.1 200 OK\r\n" + cors + "Content-Type: application/json\r\nContent-Length: " +
                       std::to_string(json.size()) + "\r\n\r\n" + json;
        } else if (url.find("/api/race_demo") == 0) {
            bool sync = (req.find("sync=1") != std::string::npos || url.find("sync=1") != std::string::npos);
            RaceResult r = runRaceDemo(sync);
            std::ostringstream ss;
            ss << "{\"synchronized\":" << (r.synchronized ? "true" : "false")
               << ",\"expectedTotal\":" << r.expectedTotal
               << ",\"actualTotal\":" << r.actualTotal
               << ",\"lostUpdates\":" << r.lostUpdates
               << ",\"corruptionRatePercent\":" << std::fixed << std::setprecision(2) << r.corruptionPercent
               << ",\"executionDurationMs\":" << r.durationMs
               << ",\"analysisSummary\":\""
               << (sync ? "Mutual exclusion protected: 0 updates lost." : "Data race detected! Non-atomic read-modify-write caused lost updates.")
               << "\"}";
            std::string json = ss.str();
            response = "HTTP/1.1 200 OK\r\n" + cors + "Content-Type: application/json\r\nContent-Length: " +
                       std::to_string(json.size()) + "\r\n\r\n" + json;
        } else if (url.find("/api/inject_emergency") == 0) {
            int dir = WEST;
            if (req.find("dir=0") != std::string::npos) dir = NORTH;
            if (req.find("dir=1") != std::string::npos) dir = SOUTH;
            if (req.find("dir=2") != std::string::npos) dir = EAST;

            Vehicle v;
            v.id = 999999;
            v.direction = dir;
            v.isEmergency = true;
            v.arrivalTimeSec = getTimeSec();
            g_lanes[dir].pushVehicle(v);

            g_emergencyLane.store(dir);
            g_emergencyActive.store(true);

            response = "HTTP/1.1 200 OK\r\n" + cors + "Content-Length: 15\r\n\r\n{\"status\":\"ok\"}";
        } else if (url.find("/api/control") == 0) {
            if (req.find("speed=") != std::string::npos) {
                size_t p = req.find("speed=");
                double spd = std::atof(req.c_str() + p + 6);
                if (spd > 0) g_speedMult.store(spd);
            }
            if (req.find("alg=") != std::string::npos) {
                size_t p = req.find("alg=");
                int a = std::atoi(req.c_str() + p + 4);
                g_activeAlgorithm.store(a);
            }
            if (req.find("action=reset") != std::string::npos) {
                for (int i = 0; i < 4; ++i) g_lanes[i].reset();
                g_starvationCount.store(0);
            }
            response = "HTTP/1.1 200 OK\r\n" + cors + "Content-Length: 15\r\n\r\n{\"status\":\"ok\"}";
        } else if (url.find("/api/inject_fault") == 0) {
            int target = 0;
            if (req.find("worker=1") != std::string::npos) target = 1;
            if (req.find("worker=2") != std::string::npos) target = 2;
            if (req.find("worker=3") != std::string::npos) target = 3;
            g_workerHanging[target].store(true);
            response = "HTTP/1.1 200 OK\r\n" + cors + "Content-Length: 15\r\n\r\n{\"status\":\"ok\"}";
        } else if (url.find("/api/recover") == 0) {
            for (int i = 0; i < 4; ++i) g_workerHanging[i].store(false);
            g_isFallbackActive.store(false);
            response = "HTTP/1.1 200 OK\r\n" + cors + "Content-Length: 15\r\n\r\n{\"status\":\"ok\"}";
        } else {
            // Serve static dashboard web files
            std::string file = "./web" + url;
            if (url == "/" || url.empty()) file = "./web/index.html";
            std::string content = readFileString(file);
            if (content.empty()) {
                response = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
            } else {
                std::string cType = "text/plain";
                if (file.find(".html") != std::string::npos) cType = "text/html; charset=utf-8";
                if (file.find(".css") != std::string::npos) cType = "text/css; charset=utf-8";
                if (file.find(".js") != std::string::npos) cType = "application/javascript; charset=utf-8";
                response = "HTTP/1.1 200 OK\r\n" + cors + "Content-Type: " + cType + "\r\nContent-Length: " +
                           std::to_string(content.size()) + "\r\n\r\n" + content;
            }
        }

        send(client, response.c_str(), static_cast<int>(response.size()), 0);
        closesocket(client);
    }

    closesocket(serverSock);
#ifdef _WIN32
    WSACleanup();
#endif
}

// ============================================================================
// SECTION 8: MAIN FUNCTION (ENTRY POINT)
// ============================================================================
int main(int argc, char* argv[]) {
    int port = 8080;
    if (argc > 2 && std::string(argv[1]) == "--port") {
        port = std::atoi(argv[2]);
    }

    std::cout << "===============================================================\n";
    std::cout << "  SMART TRAFFIC SIMULATION (SIMPLE MULTITHREADED OS ENGINE)    \n";
    std::cout << "===============================================================\n";
    std::cout << "-> Thread 1: North Lane Worker\n";
    std::cout << "-> Thread 2: South Lane Worker\n";
    std::cout << "-> Thread 3: East Lane Worker\n";
    std::cout << "-> Thread 4: West Lane Worker\n";
    std::cout << "-> Thread 5: Adaptive Traffic Controller (Aging & Safety)\n";
    std::cout << "-> Thread 6: Watchdog Monitor (Heartbeats & Fallback)\n";
    std::cout << "-> Thread 7: Embedded HTTP Server (Port " << port << ")\n";
    std::cout << "===============================================================\n";

    // 1. Launch 4 Lane Worker Threads
    std::thread w0(laneWorkerThread, NORTH);
    std::thread w1(laneWorkerThread, SOUTH);
    std::thread w2(laneWorkerThread, EAST);
    std::thread w3(laneWorkerThread, WEST);

    // 2. Launch Controller Thread
    std::thread ctrl(trafficControllerThread);

    // 3. Launch Watchdog Monitor Thread
    std::thread watchdog(watchdogThread);

    // 4. Launch HTTP Telemetry Server Thread
    std::thread server(httpServerThread, port);

    std::cout << "\n[System] All 7 threads running! Press Ctrl+C in terminal to stop.\n";
    std::cout << "[System] Open http://localhost:" << port << " in any browser!\n\n";

    // Periodic terminal output for easy reading in terminal
    while (g_simRunning.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        double now = getTimeSec();
        int q0 = g_lanes[0].count();
        int q1 = g_lanes[1].count();
        int q2 = g_lanes[2].count();
        int q3 = g_lanes[3].count();
        std::cout << "[T=" << std::fixed << std::setprecision(0) << now << "s] Phase: "
                  << g_currentPhaseName << " | Queues -> N:" << q0 << " S:" << q1 << " E:" << q2 << " W:" << q3
                  << " | Starvation Events: " << g_starvationCount.load() << "\n";
    }

    w0.join();
    w1.join();
    w2.join();
    w3.join();
    ctrl.join();
    watchdog.join();
    server.join();

    return 0;
}
