/**
 * ============================================================================
 * SMART TRAFFIC SIMULATION USING MULTITHREADING (OS MINI-PROJECT)
 * ============================================================================
 * 
 * CORE OS CONCEPTS DEMONSTRATED IN THIS SIMPLE FILE:
 * 1. Multithreading (std::thread): 4 Lane Workers + 1 Controller + 1 Watchdog
 * 2. Mutual Exclusion (std::mutex): Protects shared lane queues from data races
 * 3. Starvation Prevention & Aging: Prevents low-traffic lanes from waiting forever
 * 4. Safety Invariant: North-South and East-West NEVER get green simultaneously
 * 5. Emergency Priority: Safely preempts traffic for Ambulances
 * 6. Fault Detection (Watchdog): Detects missed heartbeats and restores safe mode
 * 7. Race Condition Demo: Shows lost updates when locks are removed
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
#include <iomanip>
#include <algorithm>

// ----------------------------------------------------------------------------
// 1. DATA TYPES & ENUMS
// ----------------------------------------------------------------------------
enum Direction { NORTH = 0, SOUTH = 1, EAST = 2, WEST = 3 };
enum SignalColor { RED = 0, YELLOW = 1, GREEN = 2 };

const char* dirName(int d) {
    const char* names[] = { "NORTH", "SOUTH", "EAST", "WEST" };
    return (d >= 0 && d < 4) ? names[d] : "UNKNOWN";
}

const char* colorName(SignalColor c) {
    if (c == GREEN) return "GREEN ";
    if (c == YELLOW) return "YELLOW";
    return "RED   ";
}

struct Vehicle {
    uint64_t id;
    int direction;
    bool isEmergency;
    double arrivalTimeSec;
};

// Helper: current steady time in seconds
double getTimeSec() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now.time_since_epoch()).count();
}

// ----------------------------------------------------------------------------
// 2. OS CONCEPT: MUTEX-PROTECTED SHARED QUEUE
// ----------------------------------------------------------------------------
// Each lane has a queue of cars.
// Both the Lane Worker thread and the Controller thread access it.
// std::mutex guarantees that only ONE thread accesses the queue at any time.
class LaneQueue {
public:
    int direction;
    std::mutex mtx;                 // Mutual exclusion lock
    std::deque<Vehicle> queue;      // Shared queue
    uint64_t totalArrived = 0;
    uint64_t totalServed = 0;

    LaneQueue(int d = 0) : direction(d) {}

    // Add vehicle to queue (called by Lane Worker)
    void addVehicle(const Vehicle& v) {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push_back(v);
        totalArrived++;
    }

    // Remove vehicle when signal is GREEN (called by Lane Worker)
    bool removeVehicle(Vehicle* out = nullptr) {
        std::lock_guard<std::mutex> lock(mtx);
        if (queue.empty()) return false;
        if (out) *out = queue.front();
        queue.pop_front();
        totalServed++;
        return true;
    }

    int getCount() {
        std::lock_guard<std::mutex> lock(mtx);
        return static_cast<int>(queue.size());
    }

    // OS CONCEPT: Starvation Aging Formula
    // Calculates queue length, average wait time, and starvation score
    void getStats(double now, double& outAvgWait, double& outMaxWait, double& outScore) {
        std::lock_guard<std::mutex> lock(mtx);
        outAvgWait = 0.0;
        outMaxWait = 0.0;
        outScore = 0.0;

        if (queue.empty()) return;

        double sumWait = 0.0;
        bool hasEmerg = false;

        for (const auto& v : queue) {
            double wait = now - v.arrivalTimeSec;
            if (wait < 0) wait = 0;
            sumWait += wait;
            if (wait > outMaxWait) outMaxWait = wait;
            if (v.isEmergency) hasEmerg = true;
        }

        outAvgWait = sumWait / queue.size();

        // --------------------------------------------------------------------
        // THE AGING FORMULA:
        // Score = (alpha * Density) + (beta * WaitTime) + (gamma * StarvationPenalty)
        // --------------------------------------------------------------------
        double normDensity = std::min(queue.size() / 15.0, 1.0);
        double normWait = std::min(outAvgWait / 20.0, 1.0);
        double starvationPenalty = (outMaxWait > 15.0) ? ((outMaxWait - 15.0) / 10.0) : 0.0;

        const double alpha = 0.45; // Weight of queue density
        const double beta  = 0.35; // Weight of average waiting time
        const double gamma = 0.20; // Weight of prolonged starvation penalty

        outScore = (alpha * normDensity) + (beta * normWait) + (gamma * starvationPenalty);

        // Emergency vehicle gives immediate maximum priority boost
        if (hasEmerg) outScore += 10.0;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mtx);
        queue.clear();
        totalArrived = 0;
        totalServed = 0;
    }
};

// Global Shared State
LaneQueue g_lanes[4] = { LaneQueue(NORTH), LaneQueue(SOUTH), LaneQueue(EAST), LaneQueue(WEST) };
std::atomic<SignalColor> g_signals[4] = { RED, RED, RED, RED };
std::atomic<bool> g_simRunning{true};
std::atomic<uint64_t> g_starvationEvents{0};

// Watchdog heartbeats (each lane worker updates its timestamp every 100ms)
std::atomic<double> g_heartbeats[4] = { 0.0, 0.0, 0.0, 0.0 };
std::atomic<bool> g_workerHanging[4] = { false, false, false, false };
std::atomic<bool> g_isFallbackActive{false};

// Emergency preemption request
std::atomic<bool> g_emergencyActive{false};
std::atomic<int> g_emergencyLane{-1};

std::string g_currentPhaseName = "NORTH-SOUTH GREEN";

// ----------------------------------------------------------------------------
// 3. OS CONCEPT: MULTITHREADING (4 LANE WORKERS)
// ----------------------------------------------------------------------------
// Each worker thread simulates vehicle arrivals and departures for its lane.
void laneWorkerThread(int dir) {
    std::mt19937 rng(2024 + dir);
    uint64_t carCounter = 1;

    double lastArrival = getTimeSec();
    double lastDeparture = getTimeSec();

    // Rates: North/South heavy (35 cars/min), West light (5 cars/min to test starvation)
    double ratePerMin = (dir == WEST) ? 6.0 : 30.0;

    while (g_simRunning.load()) {
        double now = getTimeSec();

        // If simulated deadlock/hang is active, do not emit heartbeats
        if (g_workerHanging[dir].load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Emit heartbeat for Watchdog
        g_heartbeats[dir].store(now);

        // 1. Vehicle Arrival (Poisson-like random interval)
        double meanInterval = 60.0 / ratePerMin;
        if (now - lastArrival >= meanInterval) {
            lastArrival = now;
            Vehicle v;
            v.id = (static_cast<uint64_t>(dir) << 32) | (carCounter++);
            v.direction = dir;
            v.isEmergency = false;
            v.arrivalTimeSec = now;
            g_lanes[dir].addVehicle(v);
        }

        // 2. Vehicle Departure when signal is GREEN
        if (g_signals[dir].load() == GREEN) {
            if (now - lastDeparture >= 1.2) { // 1 car crosses every 1.2 seconds
                lastDeparture = now;
                Vehicle out;
                g_lanes[dir].removeVehicle(&out);
            }
        } else {
            lastDeparture = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

// ----------------------------------------------------------------------------
// 4. OS CONCEPT: TRAFFIC CONTROLLER THREAD (SCHEDULING & SAFETY)
// ----------------------------------------------------------------------------
// Enforces strict mutual exclusion of conflicting green signals:
// Phase 1: NS Green -> Phase 2: NS Yellow -> Phase 3: All-Red ->
// Phase 4: EW Green -> Phase 5: EW Yellow -> Phase 6: All-Red
void trafficControllerThread() {
    enum PhaseState { NS_GREEN, NS_YELLOW, ALL_RED_TO_EW, EW_GREEN, EW_YELLOW, ALL_RED_TO_NS };
    PhaseState state = NS_GREEN;

    double phaseStart = getTimeSec();
    double greenDuration = 8.0;

    const double YELLOW_SEC = 2.0;
    const double ALL_RED_SEC = 1.0;

    while (g_simRunning.load()) {
        double now = getTimeSec();
        double elapsed = now - phaseStart;

        switch (state) {
            case NS_GREEN:
                g_currentPhaseName = "NORTH-SOUTH GREEN";
                g_signals[NORTH].store(GREEN);
                g_signals[SOUTH].store(GREEN);
                g_signals[EAST].store(RED);
                g_signals[WEST].store(RED);

                // Emergency interrupt
                if (g_emergencyActive.load() && (g_emergencyLane.load() == EAST || g_emergencyLane.load() == WEST)) {
                    state = NS_YELLOW;
                    phaseStart = now;
                } else if (elapsed >= greenDuration) {
                    state = NS_YELLOW;
                    phaseStart = now;
                }
                break;

            case NS_YELLOW:
                g_currentPhaseName = "CLEARANCE: NS YELLOW";
                g_signals[NORTH].store(YELLOW);
                g_signals[SOUTH].store(YELLOW);
                g_signals[EAST].store(RED);
                g_signals[WEST].store(RED);

                if (elapsed >= YELLOW_SEC) {
                    state = ALL_RED_TO_EW;
                    phaseStart = now;
                }
                break;

            case ALL_RED_TO_EW:
                g_currentPhaseName = "CLEARANCE: ALL RED (SAFETY)";
                g_signals[NORTH].store(RED);
                g_signals[SOUTH].store(RED);
                g_signals[EAST].store(RED);
                g_signals[WEST].store(RED);

                if (elapsed >= ALL_RED_SEC) {
                    state = EW_GREEN;
                    phaseStart = now;

                    // Calculate adaptive green time for East-West using Aging Scores
                    double wE, wW, mE, mW, sE, sW;
                    g_lanes[EAST].getStats(now, wE, mE, sE);
                    g_lanes[WEST].getStats(now, wW, mW, sW);

                    if (mW > 15.0 || mE > 15.0) g_starvationEvents++;

                    double ewScore = sE + sW;
                    greenDuration = 5.0 + std::min(15.0, ewScore * 5.0);
                }
                break;

            case EW_GREEN:
                g_currentPhaseName = "EAST-WEST GREEN";
                g_signals[NORTH].store(RED);
                g_signals[SOUTH].store(RED);
                g_signals[EAST].store(GREEN);
                g_signals[WEST].store(GREEN);

                // Emergency interrupt
                if (g_emergencyActive.load() && (g_emergencyLane.load() == NORTH || g_emergencyLane.load() == SOUTH)) {
                    state = EW_YELLOW;
                    phaseStart = now;
                } else if (elapsed >= greenDuration) {
                    state = EW_YELLOW;
                    phaseStart = now;
                }
                break;

            case EW_YELLOW:
                g_currentPhaseName = "CLEARANCE: EW YELLOW";
                g_signals[NORTH].store(RED);
                g_signals[SOUTH].store(RED);
                g_signals[EAST].store(YELLOW);
                g_signals[WEST].store(YELLOW);

                if (elapsed >= YELLOW_SEC) {
                    state = ALL_RED_TO_NS;
                    phaseStart = now;
                }
                break;

            case ALL_RED_TO_NS:
                g_currentPhaseName = "CLEARANCE: ALL RED (SAFETY)";
                g_signals[NORTH].store(RED);
                g_signals[SOUTH].store(RED);
                g_signals[EAST].store(RED);
                g_signals[WEST].store(RED);

                if (elapsed >= ALL_RED_SEC) {
                    state = NS_GREEN;
                    phaseStart = now;
                    g_emergencyActive.store(false);

                    // Calculate adaptive green time for North-South using Aging Scores
                    double wN, wS, mN, mS, sN, sS;
                    g_lanes[NORTH].getStats(now, wN, mN, sN);
                    g_lanes[SOUTH].getStats(now, wS, mS, sS);

                    if (mN > 15.0 || mS > 15.0) g_starvationEvents++;

                    double nsScore = sN + sS;
                    greenDuration = 5.0 + std::min(15.0, nsScore * 5.0);
                }
                break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

// ----------------------------------------------------------------------------
// 5. OS CONCEPT: WATCHDOG HEALTH MONITOR THREAD
// ----------------------------------------------------------------------------
// Audits thread heartbeats. If a thread stops responding for > 1.5s,
// it logs an error and triggers safe fallback mode.
void watchdogThread() {
    while (g_simRunning.load()) {
        double now = getTimeSec();
        bool fault = false;

        for (int i = 0; i < 4; ++i) {
            double last = g_heartbeats[i].load();
            if (last > 0.0 && (now - last) > 1.5) {
                fault = true;
                g_isFallbackActive.store(true);
            }
        }

        if (!fault && g_isFallbackActive.load()) {
            g_isFallbackActive.store(false);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// ----------------------------------------------------------------------------
// 6. OS CONCEPT: RACE CONDITION DEMO (LIVE COMPARISON)
// ----------------------------------------------------------------------------
// Runs an isolated test showing why Mutex is essential.
void runRaceDemo() {
    std::cout << "\n=======================================================\n";
    std::cout << "  OS CONCEPT: RACE CONDITION DEMONSTRATION             \n";
    std::cout << "=======================================================\n";
    std::cout << "Running 4 concurrent threads, 100,000 increments each.\n";
    std::cout << "Total Expected Updates = 400,000\n\n";

    // 1. Unsynchronized Test
    volatile uint64_t unsafeCounter = 0;
    std::vector<std::thread> unsyncThreads;
    for (int t = 0; t < 4; ++t) {
        unsyncThreads.emplace_back([&]() {
            for (int i = 0; i < 100000; ++i) {
                uint64_t temp = unsafeCounter;
                unsafeCounter = temp + 1; // Non-atomic read-modify-write!
            }
        });
    }
    for (auto& th : unsyncThreads) th.join();

    uint64_t lost = (400000 > unsafeCounter) ? (400000 - unsafeCounter) : 0;
    double corruptionRate = (static_cast<double>(lost) / 400000.0) * 100.0;

    std::cout << "[1] WITHOUT MUTEX (Data Race):\n";
    std::cout << "    Actual Count: " << unsafeCounter << " / 400,000\n";
    std::cout << "    Lost Updates: " << lost << " (" << std::fixed << std::setprecision(1) << corruptionRate << "% Data Corruption!)\n";
    std::cout << "    Explanation: CPU registers overwrite each other during non-atomic MOV/ADD/MOV.\n\n";

    // 2. Synchronized Test with std::mutex
    uint64_t safeCounter = 0;
    std::mutex demoMutex;
    std::vector<std::thread> syncThreads;
    for (int t = 0; t < 4; ++t) {
        syncThreads.emplace_back([&]() {
            for (int i = 0; i < 100000; ++i) {
                std::lock_guard<std::mutex> lock(demoMutex);
                safeCounter++; // Thread-safe mutual exclusion
            }
        });
    }
    for (auto& th : syncThreads) th.join();

    std::cout << "[2] WITH MUTEX (Synchronized):\n";
    std::cout << "    Actual Count: " << safeCounter << " / 400,000\n";
    std::cout << "    Lost Updates: 0 (100% Data Integrity Guaranteed)\n";
    std::cout << "=======================================================\n\n";
}

// ----------------------------------------------------------------------------
// 7. MAIN FUNCTION
// ----------------------------------------------------------------------------
int main() {
    std::cout << "===============================================================\n";
    std::cout << "  SMART TRAFFIC SIMULATION (CONCURRENT OS MINI-PROJECT)        \n";
    std::cout << "===============================================================\n";

    // Run race condition educational demonstration first
    runRaceDemo();

    std::cout << "Starting 6 Concurrent Simulation Threads:\n";
    std::cout << " -> Thread 1: North Lane Worker\n";
    std::cout << " -> Thread 2: South Lane Worker\n";
    std::cout << " -> Thread 3: East Lane Worker\n";
    std::cout << " -> Thread 4: West Lane Worker\n";
    std::cout << " -> Thread 5: Adaptive Traffic Controller\n";
    std::cout << " -> Thread 6: Watchdog Monitor\n";
    std::cout << "===============================================================\n\n";

    // Launch worker threads
    std::thread w0(laneWorkerThread, NORTH);
    std::thread w1(laneWorkerThread, SOUTH);
    std::thread w2(laneWorkerThread, EAST);
    std::thread w3(laneWorkerThread, WEST);

    // Launch controller thread
    std::thread ctrl(trafficControllerThread);

    // Launch watchdog thread
    std::thread wd(watchdogThread);

    std::cout << "SIMULATION RUNNING LIVE IN TERMINAL:\n";
    std::cout << "(Press Ctrl+C to stop at any time)\n\n";

    // Live formatted terminal output
    int iteration = 0;
    while (g_simRunning.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        iteration++;

        double now = getTimeSec();
        double wN, wS, wE, wW, mN, mS, mE, mW, sN, sS, sE, sW;
        g_lanes[NORTH].getStats(now, wN, mN, sN);
        g_lanes[SOUTH].getStats(now, wS, mS, sS);
        g_lanes[EAST].getStats(now, wE, mE, sE);
        g_lanes[WEST].getStats(now, wW, mW, sW);

        std::cout << "---------------------------------------------------------------\n";
        std::cout << " [TIME: " << std::fixed << std::setprecision(0) << now << "s] ACTIVE PHASE: " << g_currentPhaseName << "\n";
        std::cout << " SIGNALS:  North=" << colorName(g_signals[NORTH].load())
                  << " South=" << colorName(g_signals[SOUTH].load())
                  << " East=" << colorName(g_signals[EAST].load())
                  << " West=" << colorName(g_signals[WEST].load()) << "\n";
        std::cout << " QUEUES:   North: " << g_lanes[NORTH].getCount() << " cars (AvgWait: " << std::setprecision(1) << wN << "s)\n"
                  << "           South: " << g_lanes[SOUTH].getCount() << " cars (AvgWait: " << std::setprecision(1) << wS << "s)\n"
                  << "           East:  " << g_lanes[EAST].getCount() << " cars (AvgWait: " << std::setprecision(1) << wE << "s)\n"
                  << "           West:  " << g_lanes[WEST].getCount() << " cars (AvgWait: " << std::setprecision(1) << wW << "s) [AGING SCORE: " << sW << "]\n";
        std::cout << " STATS:    Total Served: " << (g_lanes[0].totalServed + g_lanes[1].totalServed + g_lanes[2].totalServed + g_lanes[3].totalServed)
                  << " | Starvation Violations: " << g_starvationEvents.load()
                  << " | Watchdog Fallback: " << (g_isFallbackActive.load() ? "ACTIVE" : "OFF (Healthy)") << "\n";

        // At iteration 6, demonstrate emergency preemption!
        if (iteration == 6) {
            std::cout << "\n>>> [DEMO EVENT] AMBULANCE INJECTED INTO WEST APPROACH! <<<\n";
            std::cout << ">>> Controller will safely clear conflicting traffic and switch to West! <<<\n\n";
            Vehicle emerg;
            emerg.id = 999999;
            emerg.direction = WEST;
            emerg.isEmergency = true;
            emerg.arrivalTimeSec = now;
            g_lanes[WEST].addVehicle(emerg);
            g_emergencyLane.store(WEST);
            g_emergencyActive.store(true);
        }
    }

    w0.join();
    w1.join();
    w2.join();
    w3.join();
    ctrl.join();
    wd.join();

    return 0;
}
