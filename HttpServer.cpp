#include "HttpServer.h"
#include "RaceConditionDemo.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

namespace TrafficSim {

HttpServer::HttpServer(int port,
                       TrafficController* controller,
                       LaneWorker* workers[4],
                       WatchdogMonitor* watchdog,
                       EmergencyHandler* emergencyHandler,
                       ExperimentManager* expManager,
                       const std::string& webRoot)
    : m_port(port),
      m_controller(controller),
      m_watchdog(watchdog),
      m_emergencyHandler(emergencyHandler),
      m_expManager(expManager),
      m_webRoot(webRoot) {
    for (int i = 0; i < 4; ++i) {
        m_workers[i] = workers[i];
    }
}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return false;
    }
#endif

    SOCKET serverSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSock == INVALID_SOCKET) {
        std::cerr << "Socket creation failed." << std::endl;
        return false;
    }

    int opt = 1;
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(static_cast<u_short>(m_port));

    if (bind(serverSock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed on port " << m_port << std::endl;
        closesocket(serverSock);
        return false;
    }

    if (listen(serverSock, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Listen failed." << std::endl;
        closesocket(serverSock);
        return false;
    }

    m_serverSocket = serverSock;
    m_running.store(true);
    m_thread = std::thread(&HttpServer::serverLoop, this);
    std::cout << "[HttpServer] Started successfully on http://localhost:" << m_port << std::endl;
    return true;
}

void HttpServer::stop() {
    if (!m_running.load()) return;
    m_running.store(false);

    if (m_serverSocket != 0) {
        closesocket(static_cast<SOCKET>(m_serverSocket));
        m_serverSocket = 0;
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

#ifdef _WIN32
    WSACleanup();
#endif
}

void HttpServer::serverLoop() {
    while (m_running.load()) {
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);
        SOCKET clientSock = accept(static_cast<SOCKET>(m_serverSocket), (sockaddr*)&clientAddr, &clientLen);
        if (clientSock == INVALID_SOCKET) {
            if (!m_running.load()) break;
            continue;
        }

        // Handle each HTTP request in a detached thread for low latency
        std::thread(&HttpServer::handleClient, this, static_cast<uintptr_t>(clientSock)).detach();
    }
}

void HttpServer::handleClient(uintptr_t clientSocket) {
    SOCKET sock = static_cast<SOCKET>(clientSocket);
    char buffer[4096];
    int bytesRead = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (bytesRead <= 0) {
        closesocket(sock);
        return;
    }
    buffer[bytesRead] = '\0';

    std::string requestStr(buffer);
    std::istringstream reqStream(requestStr);
    std::string method, path, version;
    reqStream >> method >> path >> version;

    // Read headers until double CRLF
    size_t bodyPos = requestStr.find("\r\n\r\n");
    std::string body = "";
    if (bodyPos != std::string::npos && bodyPos + 4 < requestStr.size()) {
        body = requestStr.substr(bodyPos + 4);
    }

    // Process request
    std::string response = handleRequest(method, path, body);
    send(sock, response.c_str(), static_cast<int>(response.size()), 0);
    closesocket(sock);
}

std::string HttpServer::getContentType(const std::string& path) {
    if (path.rfind(".html") != std::string::npos) return "text/html; charset=utf-8";
    if (path.rfind(".css") != std::string::npos) return "text/css; charset=utf-8";
    if (path.rfind(".js") != std::string::npos) return "application/javascript; charset=utf-8";
    if (path.rfind(".json") != std::string::npos) return "application/json; charset=utf-8";
    if (path.rfind(".csv") != std::string::npos) return "text/csv; charset=utf-8";
    if (path.rfind(".svg") != std::string::npos) return "image/svg+xml";
    return "text/plain";
}

std::string HttpServer::readFileContents(const std::string& fullPath) {
    std::ifstream file(fullPath, std::ios::in | std::ios::binary);
    if (!file.is_open()) return "";
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::string HttpServer::snapshotToJson(const IntersectionSnapshot& snap) {
    std::ostringstream ss;
    ss << "{"
       << "\"simTime\":" << std::fixed << std::setprecision(2) << snap.simTimeSec << ","
       << "\"isRunning\":" << (snap.isRunning ? "true" : "false") << ","
       << "\"isPaused\":" << (snap.isPaused ? "true" : "false") << ","
       << "\"algorithm\":" << static_cast<int>(snap.algorithm) << ","
       << "\"algorithmName\":\"" << algorithmToString(snap.algorithm) << "\","
       << "\"syncStrategy\":" << static_cast<int>(snap.syncStrategy) << ","
       << "\"syncStrategyName\":\"" << syncStrategyToString(snap.syncStrategy) << "\","
       << "\"currentPhase\":" << static_cast<int>(snap.currentPhase) << ","
       << "\"currentPhaseName\":\"" << phaseToString(snap.currentPhase) << "\","
       << "\"phaseElapsedSec\":" << snap.phaseElapsedSec << ","
       << "\"phaseDurationSec\":" << snap.currentPhaseDurationSec << ",";

    // Signals
    ss << "\"signals\":[";
    for (int i = 0; i < 4; ++i) {
        ss << "\"" << signalColorToString(snap.signals[i]) << "\"" << (i < 3 ? "," : "");
    }
    ss << "],";

    // Lanes
    ss << "\"lanes\":[";
    for (int i = 0; i < 4; ++i) {
        const auto& l = snap.lanes[i];
        ss << "{"
           << "\"direction\":\"" << directionToString(static_cast<Direction>(i)) << "\","
           << "\"queueLength\":" << l.queueLength << ","
           << "\"avgWaitTimeSec\":" << l.avgWaitTimeSec << ","
           << "\"maxWaitTimeSec\":" << l.maxWaitTimeSec << ","
           << "\"totalVehiclesServed\":" << l.totalVehiclesServed << ","
           << "\"totalVehiclesArrived\":" << l.totalVehiclesArrived << ","
           << "\"starvationScore\":" << l.starvationScore << ","
           << "\"hasEmergency\":" << (l.hasEmergencyVehicle ? "true" : "false")
           << "}" << (i < 3 ? "," : "");
    }
    ss << "],";

    // Aggregate
    ss << "\"totalVehiclesProcessed\":" << snap.totalVehiclesProcessed << ","
       << "\"overallAvgWaitSec\":" << snap.overallAvgWaitSec << ","
       << "\"overallMaxWaitSec\":" << snap.overallMaxWaitSec << ","
       << "\"starvationViolationsCount\":" << snap.starvationViolationsCount << ","
       << "\"jainsFairnessIndex\":" << std::setprecision(4) << snap.jainsFairnessIndex << ","
       << "\"controllerDecisionLatencyMicrosec\":" << std::setprecision(1) << snap.controllerDecisionLatencyMicrosec << ","
       << "\"deadlineMissCount\":" << snap.deadlineMissCount << ",";

    // OS Metrics from Watchdog
    if (m_watchdog) {
        ss << "\"cpuUtilizationPercent\":" << std::setprecision(1) << m_watchdog->getCpuUsagePercent() << ","
           << "\"memoryUsageMb\":" << m_watchdog->getMemoryUsageMb() << ","
           << "\"avgSyncOverheadMicrosec\":" << m_watchdog->getAvgSyncOverheadMicrosec() << ",";
    } else {
        ss << "\"cpuUtilizationPercent\":0.0,\"memoryUsageMb\":0.0,\"avgSyncOverheadMicrosec\":0.0,";
    }

    // Safety & Faults
    ss << "\"safetyConflictDetected\":" << (snap.safetyConflictDetected ? "true" : "false") << ","
       << "\"isSafeFallbackActive\":" << (snap.isSafeFallbackActive ? "true" : "false") << ",";

    if (m_watchdog) {
        ss << "\"activeFault\":" << static_cast<int>(m_watchdog->getActiveFault()) << ","
           << "\"activeFaultName\":\"" << faultTypeToString(m_watchdog->getActiveFault()) << "\","
           << "\"faultTargetWorkerIndex\":" << m_watchdog->getFaultTargetIndex() << ",";
    } else {
        ss << "\"activeFault\":0,\"activeFaultName\":\"None\",\"faultTargetWorkerIndex\":-1,";
    }

    ss << "\"lastEmergencyLog\":\"" << snap.lastEmergencyLog << "\",";

    // Threads
    ss << "\"threads\":[";
    for (size_t i = 0; i < snap.threads.size(); ++i) {
        const auto& t = snap.threads[i];
        ss << "{"
           << "\"name\":\"" << t.threadName << "\","
           << "\"osThreadId\":" << t.osThreadId << ","
           << "\"status\":\"" << threadStatusToString(t.status) << "\","
           << "\"lastHeartbeatSec\":" << std::setprecision(2) << t.lastHeartbeatSec << ","
           << "\"lockWaitCount\":" << t.lockWaitCount << ","
           << "\"avgLockWaitMicrosec\":" << std::setprecision(1) << t.avgLockWaitMicroseconds << ","
           << "\"isHealthy\":" << (t.isHealthy ? "true" : "false")
           << "}" << (i + 1 < snap.threads.size() ? "," : "");
    }
    ss << "],";

    // Active vehicles (first 40)
    ss << "\"activeVehicles\":[";
    size_t count = std::min(snap.activeVehicles.size(), static_cast<size_t>(40));
    for (size_t i = 0; i < count; ++i) {
        const auto& v = snap.activeVehicles[i];
        ss << "{"
           << "\"id\":" << v.id << ","
           << "\"dir\":\"" << directionToString(v.direction) << "\","
           << "\"type\":\"" << vehicleTypeToString(v.type) << "\","
           << "\"isEmergency\":" << (v.isEmergency ? "true" : "false") << ","
           << "\"waitTimeSec\":" << std::setprecision(1) << (snap.simTimeSec - v.arrivalTimeSec)
           << "}" << (i + 1 < count ? "," : "");
    }
    ss << "]}";

    return ss.str();
}

std::string HttpServer::handleRequest(const std::string& method, const std::string& path, const std::string& body) {
    std::string corsHeaders = "Access-Control-Allow-Origin: *\r\n"
                              "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                              "Access-Control-Allow-Headers: Content-Type\r\n";

    if (method == "OPTIONS") {
        return "HTTP/1.1 204 No Content\r\n" + corsHeaders + "\r\n";
    }

    std::string cleanPath = path;
    std::string queryStr = "";
    size_t qPos = path.find('?');
    if (qPos != std::string::npos) {
        cleanPath = path.substr(0, qPos);
        queryStr = path.substr(qPos + 1);
    }
    std::string params = body + "&" + queryStr;

    // REST API endpoints
    if (cleanPath == "/api/status" && method == "GET") {
        if (!m_controller) return "HTTP/1.1 500 Internal Error\r\n\r\n";
        IntersectionSnapshot snap = m_controller->getSnapshot();
        std::string json = snapshotToJson(snap);
        return "HTTP/1.1 200 OK\r\n" + corsHeaders +
               "Content-Type: application/json; charset=utf-8\r\n"
               "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
    }

    if (cleanPath == "/api/control" && method == "POST") {
        SimulationConfig cfg = m_controller->getConfig();

        // Simple URL-encoded or JSON parser
        if (params.find("action=start") != std::string::npos || params.find("\"action\":\"start\"") != std::string::npos) {
            for (int i = 0; i < 4; ++i) m_workers[i]->start();
            m_controller->start();
            if (m_watchdog) m_watchdog->start();
        } else if (params.find("action=stop") != std::string::npos || params.find("\"action\":\"stop\"") != std::string::npos) {
            if (m_watchdog) m_watchdog->stop();
            m_controller->stop();
            for (int i = 0; i < 4; ++i) m_workers[i]->stop();
        } else if (params.find("action=reset") != std::string::npos || params.find("\"action\":\"reset\"") != std::string::npos) {
            if (m_watchdog) m_watchdog->reset();
            m_controller->reset();
            for (int i = 0; i < 4; ++i) m_workers[i]->reset();
            if (m_emergencyHandler) m_emergencyHandler->reset();
        }

        // Set algorithm
        if (params.find("alg=0") != std::string::npos) cfg.algorithm = AlgorithmType::BASELINE_A_FIXED_TIME;
        if (params.find("alg=1") != std::string::npos) cfg.algorithm = AlgorithmType::BASELINE_B_ADAPTIVE;
        if (params.find("alg=2") != std::string::npos) cfg.algorithm = AlgorithmType::PROPOSED_C_FAIRNESS_FAULT_TOLERANT;

        // Set speed
        auto posSpeed = params.find("speed=");
        if (posSpeed != std::string::npos) {
            double spd = std::atof(params.c_str() + posSpeed + 6);
            if (spd > 0.0) cfg.timeSpeedMultiplier = spd;
        }

        // Set arrival rate
        auto posRate = params.find("rate=");
        if (posRate != std::string::npos) {
            double rate = std::atof(params.c_str() + posRate + 5);
            if (rate >= 0.0) {
                for (int i = 0; i < 4; ++i) cfg.arrivalRatePerMin[i] = rate;
            }
        }

        m_controller->setConfig(cfg);
        std::string reply = "{\"status\":\"ok\"}";
        return "HTTP/1.1 200 OK\r\n" + corsHeaders +
               "Content-Type: application/json\r\n"
               "Content-Length: " + std::to_string(reply.size()) + "\r\n\r\n" + reply;
    }

    if (cleanPath == "/api/inject_emergency" && method == "POST") {
        Direction dir = Direction::NORTH;
        if (params.find("dir=1") != std::string::npos || params.find("dir=south") != std::string::npos) dir = Direction::SOUTH;
        if (params.find("dir=2") != std::string::npos || params.find("dir=east") != std::string::npos) dir = Direction::EAST;
        if (params.find("dir=3") != std::string::npos || params.find("dir=west") != std::string::npos) dir = Direction::WEST;

        VehicleType vt = VehicleType::AMBULANCE;
        if (params.find("type=fire") != std::string::npos) vt = VehicleType::FIRE_TRUCK;

        if (m_emergencyHandler) {
            m_emergencyHandler->dispatchEmergency(dir, vt);
        }

        std::string reply = "{\"status\":\"emergency_injected\"}";
        return "HTTP/1.1 200 OK\r\n" + corsHeaders +
               "Content-Type: application/json\r\n"
               "Content-Length: " + std::to_string(reply.size()) + "\r\n\r\n" + reply;
    }

    if (cleanPath == "/api/inject_fault" && method == "POST") {
        int workerIdx = 0;
        if (params.find("worker=1") != std::string::npos) workerIdx = 1;
        if (params.find("worker=2") != std::string::npos) workerIdx = 2;
        if (params.find("worker=3") != std::string::npos) workerIdx = 3;

        FaultType ft = FaultType::WORKER_HANG;
        if (params.find("fault=2") != std::string::npos || params.find("fault=delay") != std::string::npos) ft = FaultType::CONTROLLER_DELAY;
        if (params.find("fault=3") != std::string::npos || params.find("fault=stale") != std::string::npos) ft = FaultType::STALE_UPDATES;

        if (m_watchdog) {
            m_watchdog->injectWorkerFailure(workerIdx, ft);
        }

        std::string reply = "{\"status\":\"fault_injected\"}";
        return "HTTP/1.1 200 OK\r\n" + corsHeaders +
               "Content-Type: application/json\r\n"
               "Content-Length: " + std::to_string(reply.size()) + "\r\n\r\n" + reply;
    }

    if (cleanPath == "/api/recover" && method == "POST") {
        if (m_watchdog) {
            m_watchdog->triggerManualRecovery();
        }
        std::string reply = "{\"status\":\"recovered\"}";
        return "HTTP/1.1 200 OK\r\n" + corsHeaders +
               "Content-Type: application/json\r\n"
               "Content-Length: " + std::to_string(reply.size()) + "\r\n\r\n" + reply;
    }

    if (cleanPath == "/api/race_demo" && (method == "GET" || method == "POST")) {
        bool sync = (params.find("sync=1") != std::string::npos);
        uint32_t threads = 4;
        auto posTh = params.find("threads=");
        if (posTh != std::string::npos) {
            int th = std::atoi(params.c_str() + posTh + 8);
            if (th > 0) threads = static_cast<uint32_t>(th);
        }

        uint64_t iters = 100000;
        auto posIt = params.find("iters=");
        if (posIt != std::string::npos) {
            int64_t it = std::atoll(params.c_str() + posIt + 6);
            if (it > 0) iters = static_cast<uint64_t>(it);
        }

        RaceDemoResult r = RaceConditionDemo::runDemo(sync, threads, iters);

        std::ostringstream ss;
        ss << "{"
           << "\"synchronized\":" << (r.synchronized ? "true" : "false") << ","
           << "\"numThreads\":" << r.numThreads << ","
           << "\"iterationsPerThread\":" << r.iterationsPerThread << ","
           << "\"expectedTotal\":" << r.expectedTotal << ","
           << "\"actualTotal\":" << r.actualTotal << ","
           << "\"lostUpdates\":" << r.lostUpdates << ","
           << "\"corruptionRatePercent\":" << std::fixed << std::setprecision(2) << r.corruptionRatePercent << ","
           << "\"executionDurationMs\":" << r.executionDurationMs << ","
           << "\"analysisSummary\":\"" << r.analysisSummary << "\""
           << "}";

        std::string reply = ss.str();
        return "HTTP/1.1 200 OK\r\n" + corsHeaders +
               "Content-Type: application/json\r\n"
               "Content-Length: " + std::to_string(reply.size()) + "\r\n\r\n" + reply;
    }

    if (cleanPath == "/api/run_benchmarks" && method == "POST") {
        if (!m_expManager) return "HTTP/1.1 500 Internal Error\r\n\r\n";
        auto results = m_expManager->runComprehensiveBenchmark();

        std::ostringstream ss;
        ss << "{\"results\":[";
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            ss << "{"
               << "\"scenario\":\"" << r.testScenarioName << "\","
               << "\"algorithm\":\"" << r.algorithmName << "\","
               << "\"syncStrategy\":\"" << r.syncStrategyName << "\","
               << "\"totalServed\":" << r.totalServed << ","
               << "\"throughputPerMin\":" << std::fixed << std::setprecision(1) << r.throughputPerMin << ","
               << "\"avgWaitSec\":" << r.avgWaitSec << ","
               << "\"maxWaitSec\":" << r.maxWaitSec << ","
               << "\"jainsFairnessIndex\":" << std::setprecision(4) << r.jainsFairnessIndex << ","
               << "\"starvationEvents\":" << r.starvationEvents << ","
               << "\"avgCpuPercent\":" << std::setprecision(1) << r.avgCpuPercent << ","
               << "\"avgControllerLatencyMicrosec\":" << r.avgControllerLatencyMicrosec << ","
               << "\"avgSyncOverheadMicrosec\":" << r.avgSyncOverheadMicrosec << ","
               << "\"deadlineMisses\":" << r.deadlineMisses
               << "}" << (i + 1 < results.size() ? "," : "");
        }
        ss << "]}";

        std::string reply = ss.str();
        return "HTTP/1.1 200 OK\r\n" + corsHeaders +
               "Content-Type: application/json\r\n"
               "Content-Length: " + std::to_string(reply.size()) + "\r\n\r\n" + reply;
    }

    if (cleanPath == "/api/export_csv" && method == "GET") {
        std::string csv = m_expManager ? m_expManager->exportToCsvString() : "";
        if (csv.empty()) {
            csv = "Scenario,Algorithm,SyncStrategy,DurationSec,TotalServed,ThroughputPerMin,AvgWaitSec,MaxWaitSec,JainsFairnessIndex,StarvationEvents\n";
        }
        return "HTTP/1.1 200 OK\r\n" + corsHeaders +
               "Content-Type: text/csv\r\n"
               "Content-Disposition: attachment; filename=\"traffic_simulation_experiments.csv\"\r\n"
               "Content-Length: " + std::to_string(csv.size()) + "\r\n\r\n" + csv;
    }

    // Static file serving
    std::string filePath = cleanPath;
    if (filePath == "/" || filePath.empty()) {
        filePath = "/index.html";
    }

    // Prevent directory traversal
    if (filePath.find("..") != std::string::npos) {
        return "HTTP/1.1 403 Forbidden\r\n\r\n";
    }

    std::string fullPath = m_webRoot + filePath;
    std::string content = readFileContents(fullPath);
    if (content.empty()) {
        std::string notFound = "404 Not Found";
        return "HTTP/1.1 404 Not Found\r\nContent-Length: 13\r\n\r\n" + notFound;
    }

    std::string cType = getContentType(filePath);
    return "HTTP/1.1 200 OK\r\n" + corsHeaders +
           "Content-Type: " + cType + "\r\n"
           "Content-Length: " + std::to_string(content.size()) + "\r\n\r\n" + content;
}

} // namespace TrafficSim
