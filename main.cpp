#include "Common.h"
#include "Lane.h"
#include "LaneWorker.h"
#include "TrafficController.h"
#include "WatchdogMonitor.h"
#include "EmergencyHandler.h"
#include "ExperimentManager.h"
#include "HttpServer.h"

#include <iostream>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace TrafficSim;

static std::atomic<bool> g_keepRunning{true};

#ifdef _WIN32
BOOL WINAPI consoleCtrlHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        std::cout << "\n[System] Shutdown signal received. Stopping simulation gracefully...\n";
        g_keepRunning.store(false);
        return TRUE;
    }
    return FALSE;
}
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#endif

    int port = 8080;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        }
    }

    std::cout << "====================================================================\n";
    std::cout << "  SMART TRAFFIC SIMULATION USING MULTITHREADING (OS MINI-PROJECT)  \n";
    std::cout << "  Starvation Prevention | Fault Recovery | Synchronization Analysis  \n";
    std::cout << "====================================================================\n";

    Lane* lanes[4];
    LaneWorker* workers[4];

    for (int i = 0; i < 4; ++i) {
        lanes[i] = new Lane(static_cast<Direction>(i));
        workers[i] = new LaneWorker(static_cast<Direction>(i), lanes[i], nullptr);
    }

    TrafficController controller(lanes, workers, nullptr);
    WatchdogMonitor watchdog(&controller, workers);
    EmergencyHandler emergencyHandler(&controller, workers);
    ExperimentManager expManager;

    HttpServer server(port, &controller, workers, &watchdog, &emergencyHandler, &expManager, "./web");

    if (!server.start()) {
        std::cerr << "[Error] Could not start HTTP server on port " << port << std::endl;
        return 1;
    }

    std::cout << "\n[Simulation Engine Initialized]\n";
    std::cout << " -> 4 Lane Monitoring Threads (North, South, East, West)\n";
    std::cout << " -> 1 Adaptive Traffic Controller Thread (Fairness & Starvation Aging)\n";
    std::cout << " -> 1 Watchdog & Health Monitor Thread (CPU, Memory, Heartbeats, Fallback)\n";
    std::cout << " -> 1 Emergency Handler Dispatcher\n";
    std::cout << " -> 1 High-Performance Embedded HTTP Telemetry Server\n";
    std::cout << "\n====================================================================\n";
    std::cout << "  DASHBOARD URL: http://localhost:" << port << "\n";
    std::cout << "  Open this link in any web browser to view the interactive simulation!\n";
    std::cout << "====================================================================\n";
    std::cout << "[System] Starting simulation worker threads...\n";

    for (int i = 0; i < 4; ++i) {
        workers[i]->start();
    }
    controller.start();
    watchdog.start();

    std::cout << "[System] Simulation running. Press Ctrl+C in terminal to stop.\n\n";

    // Main thread periodic status print
    int tick = 0;
    while (g_keepRunning.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        tick += 2;
        if (tick % 10 == 0) {
            IntersectionSnapshot snap = controller.getSnapshot();
            std::cout << "[T=" << snap.simTimeSec << "s] Phase: " << phaseToString(snap.currentPhase)
                      << " | Served: " << snap.totalVehiclesProcessed
                      << " | AvgWait: " << snap.overallAvgWaitSec << "s"
                      << " | MaxWait: " << snap.overallMaxWaitSec << "s"
                      << " | CPU: " << watchdog.getCpuUsagePercent() << "%"
                      << " | Memory: " << watchdog.getMemoryUsageMb() << " MB"
                      << " | Deadlines Missed: " << snap.deadlineMissCount << std::endl;
        }
    }

    std::cout << "\n[System] Shutting down simulation...\n";
    watchdog.stop();
    controller.stop();
    for (int i = 0; i < 4; ++i) {
        workers[i]->stop();
    }
    server.stop();

    for (int i = 0; i < 4; ++i) {
        delete workers[i];
        delete lanes[i];
    }

    std::cout << "[System] Clean shutdown completed. Goodbye!\n";
    return 0;
}
