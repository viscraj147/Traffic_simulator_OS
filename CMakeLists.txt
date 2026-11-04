cmake_minimum_required(VERSION 3.15)
project(SmartTrafficSimulation VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

set(SOURCES
    src/Lane.cpp
    src/LaneWorker.cpp
    src/TrafficController.cpp
    src/EmergencyHandler.cpp
    src/WatchdogMonitor.cpp
    src/RaceConditionDemo.cpp
    src/ExperimentManager.cpp
    src/HttpServer.cpp
    src/main.cpp
)

add_executable(traffic_sim ${SOURCES})

if (WIN32)
    target_link_libraries(traffic_sim ws2_32 psapi)
else()
    target_link_libraries(traffic_sim pthread)
endif()
