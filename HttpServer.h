#pragma once

#include "Common.h"
#include "TrafficController.h"
#include "WatchdogMonitor.h"
#include "EmergencyHandler.h"
#include "ExperimentManager.h"
#include <string>
#include <thread>
#include <atomic>

namespace TrafficSim {

class HttpServer {
public:
    HttpServer(int port,
               TrafficController* controller,
               LaneWorker* workers[4],
               WatchdogMonitor* watchdog,
               EmergencyHandler* emergencyHandler,
               ExperimentManager* expManager,
               const std::string& webRoot = "./web");
    ~HttpServer();

    bool start();
    void stop();
    int getPort() const { return m_port; }

private:
    void serverLoop();
    void handleClient(uintptr_t clientSocket);
    std::string handleRequest(const std::string& method, const std::string& path, const std::string& body);

    std::string getContentType(const std::string& path);
    std::string readFileContents(const std::string& fullPath);
    std::string snapshotToJson(const IntersectionSnapshot& snap);

    int m_port;
    TrafficController* m_controller;
    LaneWorker* m_workers[4];
    WatchdogMonitor* m_watchdog;
    EmergencyHandler* m_emergencyHandler;
    ExperimentManager* m_expManager;
    std::string m_webRoot;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    uintptr_t m_serverSocket{0};
};

} // namespace TrafficSim
