#!/usr/bin/env python3
"""
===============================================================================
SMART TRAFFIC SIMULATION USING MULTITHREADING (OS MINI-PROJECT)
Simple, Self-Contained, Viva-Ready Implementation
===============================================================================

CORE OS CONCEPTS IMPLEMENTED:
1. Multithreading (threading.Thread): 4 Lane Workers, 1 Controller, 1 Watchdog, 1 Web Server.
2. Mutual Exclusion (threading.Lock): Protects shared lane queues from race conditions.
3. Starvation Prevention & Aging: Score = 0.45*Density + 0.35*Wait + 0.20*Starvation.
4. Safe Phase Transitions: Strict Green -> Yellow -> All-Red mutual exclusion.
5. Emergency Preemption: Safely transitions conflicting signals for ambulances.
6. Fault Detection & Watchdog: Audits thread heartbeats and triggers safe fallback.
7. Race Condition Demo: Shows lost updates when locks are removed.
8. Interactive Web Dashboard: Serves http://localhost:8080 without external libraries.
===============================================================================
"""

import time
import random
import threading
from collections import deque
from http.server import HTTPServer, SimpleHTTPRequestHandler
import json
import urllib.parse
import os
import sys

# -----------------------------------------------------------------------------
# 1. CONSTANTS & DATA STRUCTURES
# -----------------------------------------------------------------------------
NORTH, SOUTH, EAST, WEST = 0, 1, 2, 3
DIR_NAMES = ["NORTH", "SOUTH", "EAST", "WEST"]
RED, YELLOW, GREEN = "RED", "YELLOW", "GREEN"

class Vehicle:
    def __init__(self, vid, direction, is_emergency=False):
        self.id = vid
        self.direction = direction
        self.is_emergency = is_emergency
        self.arrival_time = time.time()

# -----------------------------------------------------------------------------
# 2. OS CONCEPT: MUTEX-PROTECTED SHARED QUEUE (SHARED MEMORY)
# -----------------------------------------------------------------------------
class LaneQueue:
    def __init__(self, direction):
        self.direction = direction
        self.lock = threading.Lock() # Mutex lock for mutual exclusion
        self.queue = deque()
        self.total_arrived = 0
        self.total_served = 0

    def add_vehicle(self, vehicle):
        with self.lock: # Acquire lock
            self.queue.append(vehicle)
            self.total_arrived += 1

    def remove_vehicle(self):
        with self.lock: # Acquire lock
            if self.queue:
                self.total_served += 1
                return self.queue.popleft()
            return None

    def count(self):
        with self.lock:
            return len(self.queue)

    def has_emergency(self):
        with self.lock:
            return any(v.is_emergency for v in self.queue)

    # OS CONCEPT: Starvation Aging Formula
    def get_stats(self, now, starvation_threshold=15.0):
        with self.lock:
            q_len = len(self.queue)
            if q_len == 0:
                return 0, 0.0, 0.0, 0.0

            total_wait = sum(max(0.0, now - v.arrival_time) for v in self.queue)
            avg_wait = total_wait / q_len
            max_wait = max(max(0.0, now - v.arrival_time) for v in self.queue)

            # Aging Formula
            norm_density = min(q_len / 15.0, 1.0)
            norm_wait = min(avg_wait / 20.0, 1.0)
            starvation_penalty = max(0.0, (max_wait - starvation_threshold) / 10.0) if max_wait > starvation_threshold else 0.0

            alpha, beta, gamma = 0.45, 0.35, 0.20
            score = (alpha * norm_density) + (beta * norm_wait) + (gamma * starvation_penalty)

            if any(v.is_emergency for v in self.queue):
                score += 10.0 # Emergency boost

            return q_len, avg_wait, max_wait, score

    def reset(self):
        with self.lock:
            self.queue.clear()
            self.total_arrived = 0
            self.total_served = 0

# -----------------------------------------------------------------------------
# GLOBAL STATE
# -----------------------------------------------------------------------------
lanes = [LaneQueue(i) for i in range(4)]
signals = [RED, RED, RED, RED]
signals_lock = threading.Lock()

sim_running = True
speed_multiplier = 1.0
active_algorithm = 2 # 0: Fixed, 1: Density, 2: Proposed Aging
is_fallback_active = False

heartbeats = [time.time()] * 4
worker_hanging = [False] * 4

emergency_active = False
emergency_lane = -1
starvation_count = 0

current_phase_name = "NORTH-SOUTH GREEN"
phase_time_remaining = 8.0

# -----------------------------------------------------------------------------
# 3. OS CONCEPT: MULTITHREADING (4 LANE WORKER THREADS)
# -----------------------------------------------------------------------------
def lane_worker(direction):
    global heartbeats
    car_id = 1
    last_arrival = time.time()
    last_departure = time.time()

    # West has low traffic (to test starvation), North/South have high traffic
    arrival_rate = 6.0 if direction == WEST else 30.0 # cars per min

    while sim_running:
        now = time.time()

        if worker_hanging[direction]:
            time.sleep(0.1)
            continue

        heartbeats[direction] = now
        speed = max(0.2, speed_multiplier)

        # 1. Arrival
        mean_interval = (60.0 / arrival_rate) / speed
        if now - last_arrival >= mean_interval:
            last_arrival = now
            v = Vehicle((direction << 32) | car_id, direction, is_emergency=False)
            car_id += 1
            lanes[direction].add_vehicle(v)

        # 2. Departure when light is GREEN
        with signals_lock:
            sig = signals[direction]

        if sig == GREEN:
            dep_interval = 1.3 / speed
            if now - last_departure >= dep_interval:
                last_departure = now
                lanes[direction].remove_vehicle()
        else:
            last_departure = now

        time.sleep(0.03)

# -----------------------------------------------------------------------------
# 4. OS CONCEPT: TRAFFIC CONTROLLER THREAD (SCHEDULING & SAFETY)
# -----------------------------------------------------------------------------
def traffic_controller():
    global current_phase_name, phase_time_remaining, starvation_count, emergency_active

    state = "NS_GREEN"
    state_start = time.time()
    green_duration = 8.0

    YELLOW_TIME = 2.0
    ALL_RED_TIME = 1.0

    while sim_running:
        now = time.time()
        speed = max(0.2, speed_multiplier)
        elapsed = (now - state_start) * speed

        with signals_lock:
            if state == "NS_GREEN":
                current_phase_name = "NORTH-SOUTH GREEN"
                signals[NORTH] = GREEN
                signals[SOUTH] = GREEN
                signals[EAST] = RED
                signals[WEST] = RED
                phase_time_remaining = max(0.0, (green_duration - elapsed) / speed)

                if emergency_active and emergency_lane in (EAST, WEST):
                    state = "NS_YELLOW"
                    state_start = now
                elif elapsed >= green_duration:
                    state = "NS_YELLOW"
                    state_start = now

            elif state == "NS_YELLOW":
                current_phase_name = "CLEARANCE: NS YELLOW"
                signals[NORTH] = YELLOW
                signals[SOUTH] = YELLOW
                signals[EAST] = RED
                signals[WEST] = RED
                phase_time_remaining = max(0.0, (YELLOW_TIME - elapsed) / speed)

                if elapsed >= YELLOW_TIME:
                    state = "ALL_RED_1"
                    state_start = now

            elif state == "ALL_RED_1":
                current_phase_name = "CLEARANCE: ALL RED (SAFETY)"
                signals[NORTH] = RED
                signals[SOUTH] = RED
                signals[EAST] = RED
                signals[WEST] = RED
                phase_time_remaining = max(0.0, (ALL_RED_TIME - elapsed) / speed)

                if elapsed >= ALL_RED_TIME:
                    state = "EW_GREEN"
                    state_start = now

                    # Compute green duration for East-West
                    _, _, mE, sE = lanes[EAST].get_stats(now)
                    _, _, mW, sW = lanes[WEST].get_stats(now)

                    if mW > 15.0 or mE > 15.0:
                        starvation_count += 1

                    if active_algorithm == 0 or is_fallback_active:
                        green_duration = 8.0 # Baseline A: Fixed
                    elif active_algorithm == 1:
                        # Baseline B: Density only
                        qE = lanes[EAST].count()
                        qW = lanes[WEST].count()
                        green_duration = 5.0 + min(15.0, (qE + qW) * 0.8)
                    else:
                        # Proposed C: Aging & Starvation Formula
                        ew_score = sE + sW
                        green_duration = 5.0 + min(15.0, ew_score * 5.0)

            elif state == "EW_GREEN":
                current_phase_name = "EAST-WEST GREEN"
                signals[NORTH] = RED
                signals[SOUTH] = RED
                signals[EAST] = GREEN
                signals[WEST] = GREEN
                phase_time_remaining = max(0.0, (green_duration - elapsed) / speed)

                if emergency_active and emergency_lane in (NORTH, SOUTH):
                    state = "EW_YELLOW"
                    state_start = now
                elif elapsed >= green_duration:
                    state = "EW_YELLOW"
                    state_start = now

            elif state == "EW_YELLOW":
                current_phase_name = "CLEARANCE: EW YELLOW"
                signals[NORTH] = RED
                signals[SOUTH] = RED
                signals[EAST] = YELLOW
                signals[WEST] = YELLOW
                phase_time_remaining = max(0.0, (YELLOW_TIME - elapsed) / speed)

                if elapsed >= YELLOW_TIME:
                    state = "ALL_RED_2"
                    state_start = now

            elif state == "ALL_RED_2":
                current_phase_name = "CLEARANCE: ALL RED (SAFETY)"
                signals[NORTH] = RED
                signals[SOUTH] = RED
                signals[EAST] = RED
                signals[WEST] = RED
                phase_time_remaining = max(0.0, (ALL_RED_TIME - elapsed) / speed)

                if elapsed >= ALL_RED_TIME:
                    state = "NS_GREEN"
                    state_start = now
                    emergency_active = False

                    # Compute green duration for North-South
                    _, _, mN, sN = lanes[NORTH].get_stats(now)
                    _, _, mS, sS = lanes[SOUTH].get_stats(now)

                    if mN > 15.0 or mS > 15.0:
                        starvation_count += 1

                    if active_algorithm == 0 or is_fallback_active:
                        green_duration = 8.0
                    elif active_algorithm == 1:
                        qN = lanes[NORTH].count()
                        qS = lanes[SOUTH].count()
                        green_duration = 5.0 + min(15.0, (qN + qS) * 0.8)
                    else:
                        ns_score = sN + sS
                        green_duration = 5.0 + min(15.0, ns_score * 5.0)

        time.sleep(0.02)

# -----------------------------------------------------------------------------
# 5. OS CONCEPT: WATCHDOG HEALTH MONITOR THREAD
# -----------------------------------------------------------------------------
def watchdog_monitor():
    global is_fallback_active
    while sim_running:
        now = time.time()
        fault = False
        for i in range(4):
            if (now - heartbeats[i]) > 1.5:
                fault = True
                is_fallback_active = True

        if not fault and is_fallback_active:
            is_fallback_active = False

        time.sleep(0.25)

# -----------------------------------------------------------------------------
# 6. OS CONCEPT: RACE CONDITION DEMO (LIVE COMPARISON)
# -----------------------------------------------------------------------------
def run_race_demo(sync_mode):
    ITERS = 2000
    EXPECTED = 4 * ITERS
    unsafe_counter = 0
    safe_counter = 0
    demo_lock = threading.Lock()

    t_start = time.time()

    def worker_unsync():
        nonlocal unsafe_counter
        for _ in range(ITERS):
            temp = unsafe_counter
            time.sleep(0.00001) # Force thread context switch to induce race condition
            unsafe_counter = temp + 1

    def worker_sync():
        nonlocal safe_counter
        for _ in range(ITERS):
            with demo_lock:
                temp = safe_counter
                time.sleep(0.00001)
                safe_counter = temp + 1

    threads = []
    target_func = worker_sync if sync_mode else worker_unsync
    for _ in range(4):
        t = threading.Thread(target=target_func)
        t.start()
        threads.append(t)

    for t in threads:
        t.join()

    duration = (time.time() - t_start) * 1000.0
    actual = safe_counter if sync_mode else unsafe_counter
    lost = max(0, EXPECTED - actual)
    corruption = (lost / EXPECTED) * 100.0

    return {
        "synchronized": sync_mode,
        "expectedTotal": EXPECTED,
        "actualTotal": actual,
        "lostUpdates": lost,
        "corruptionRatePercent": round(corruption, 2),
        "executionDurationMs": round(duration, 2),
        "analysisSummary": "Mutual exclusion protected: 0 updates lost." if sync_mode else f"Data race detected! {lost} updates were lost ({round(corruption, 1)}% corruption) due to unsynchronized concurrent access."
    }

# -----------------------------------------------------------------------------
# 7. EMBEDDED HTTP SERVER (FOR BROWSER DASHBOARD)
# -----------------------------------------------------------------------------
class TrafficHttpHandler(SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass # Suppress HTTP request logs to keep terminal clean

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        if url.path == "/api/status":
            now = time.time()
            lane_data = []
            for i in range(4):
                q, avg_w, max_w, score = lanes[i].get_stats(now)
                lane_data.append({
                    "direction": DIR_NAMES[i],
                    "queueLength": q,
                    "avgWaitTimeSec": round(avg_w, 1),
                    "maxWaitTimeSec": round(max_w, 1),
                    "totalVehiclesServed": lanes[i].total_served,
                    "starvationScore": round(score, 2),
                    "hasEmergency": lanes[i].has_emergency()
                })

            total_served = sum(l.total_served for l in lanes)
            with signals_lock:
                sig_copy = list(signals)

            payload = {
                "simTime": round(now, 1),
                "isRunning": sim_running,
                "currentPhaseName": current_phase_name,
                "phaseDurationSec": round(phase_time_remaining, 1),
                "isSafeFallbackActive": is_fallback_active,
                "starvationViolationsCount": starvation_count,
                "totalVehiclesProcessed": total_served,
                "signals": sig_copy,
                "lanes": lane_data,
                "threads": [
                    {"name": f"{DIR_NAMES[i]}_LaneWorker", "status": "FAILED" if worker_hanging[i] else "RUNNING", "isHealthy": not worker_hanging[i]}
                    for i in range(4)
                ] + [
                    {"name": "TrafficController", "status": "RUNNING", "isHealthy": True},
                    {"name": "WatchdogMonitor", "status": "RUNNING", "isHealthy": True}
                ]
            }

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps(payload).encode('utf-8'))

        elif url.path == "/api/race_demo":
            params = urllib.parse.parse_qs(url.query)
            sync = params.get("sync", ["0"])[0] == "1"
            res = run_race_demo(sync)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps(res).encode('utf-8'))
        else:
            # Serve files from ./web
            file_path = url.path.lstrip("/")
            if not file_path:
                file_path = "index.html"
            full_path = os.path.join(os.getcwd(), "web", file_path)

            if os.path.exists(full_path) and os.path.isfile(full_path):
                self.send_response(200)
                if full_path.endswith(".html"):
                    self.send_header("Content-Type", "text/html; charset=utf-8")
                elif full_path.endswith(".css"):
                    self.send_header("Content-Type", "text/css; charset=utf-8")
                elif full_path.endswith(".js"):
                    self.send_header("Content-Type", "application/javascript; charset=utf-8")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                with open(full_path, "rb") as f:
                    self.wfile.write(f.read())
            else:
                self.send_response(404)
                self.end_headers()
                self.wfile.write(b"404 Not Found")

    def do_POST(self):
        global emergency_active, emergency_lane, speed_multiplier, active_algorithm, is_fallback_active
        content_len = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_len).decode('utf-8')
        params = urllib.parse.parse_qs(body)

        url = urllib.parse.urlparse(self.path)

        if url.path == "/api/inject_emergency":
            dir_val = int(params.get("dir", [3])[0])
            v = Vehicle(999999, dir_val, is_emergency=True)
            lanes[dir_val].add_vehicle(v)
            emergency_lane = dir_val
            emergency_active = True

        elif url.path == "/api/control":
            if "speed" in params:
                speed_multiplier = float(params["speed"][0])
            if "alg" in params:
                active_algorithm = int(params["alg"][0])
            if "action" in params and params["action"][0] == "reset":
                for l in lanes:
                    l.reset()

        elif url.path == "/api/inject_fault":
            w = int(params.get("worker", [0])[0])
            worker_hanging[w] = True

        elif url.path == "/api/recover":
            for i in range(4):
                worker_hanging[i] = False
            is_fallback_active = False

        elif url.path == "/api/race_demo":
            sync = params.get("sync", ["0"])[0] == "1"
            res = run_race_demo(sync)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps(res).encode('utf-8'))
            return

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(b'{"status":"ok"}')

def start_web_server(port=8080):
    server = HTTPServer(("0.0.0.0", port), TrafficHttpHandler)
    server.serve_forever()

# -----------------------------------------------------------------------------
# 8. MAIN EXECUTION & TERMINAL DISPLAY
# -----------------------------------------------------------------------------
def main():
    port = 8080
    if len(sys.argv) > 2 and sys.argv[1] == "--port":
        port = int(sys.argv[2])

    print("=================================================================")
    print("  SMART TRAFFIC SIMULATION (CONCURRENT OS MINI-PROJECT)")
    print("=================================================================")
    print("-> 4 Lane Worker Threads (North, South, East, West)")
    print("-> 1 Adaptive Traffic Controller Thread (Aging & Starvation Prevention)")
    print("-> 1 Watchdog Health Monitor Thread (Fault Detection & Fallback)")
    print(f"-> 1 Web Telemetry Server running at http://localhost:{port}")
    print("=================================================================\n")

    # 1. Start Lane Workers
    for i in range(4):
        t = threading.Thread(target=lane_worker, args=(i,), daemon=True)
        t.start()

    # 2. Start Controller
    t_ctrl = threading.Thread(target=traffic_controller, daemon=True)
    t_ctrl.start()

    # 3. Start Watchdog
    t_wd = threading.Thread(target=watchdog_monitor, daemon=True)
    t_wd.start()

    # 4. Start HTTP Server
    t_http = threading.Thread(target=start_web_server, args=(port,), daemon=True)
    t_http.start()

    print(f"[READY] Web Dashboard live at: http://localhost:{port}")
    print("Press Ctrl+C to terminate.\n")

    # Terminal status display loop
    try:
        step = 0
        while True:
            time.sleep(2.0)
            step += 1
            now = time.time()

            # Trigger automated demo ambulance at step 5
            if step == 5:
                print("\n>>> [DEMO EVENT] AMBULANCE INJECTED INTO WEST APPROACH! <<<")
                print(">>> Controller will clear conflicting traffic and switch to West! <<<\n")
                lanes[WEST].add_vehicle(Vehicle(999999, WEST, is_emergency=True))
                global emergency_active, emergency_lane
                emergency_lane = WEST
                emergency_active = True

            print("-----------------------------------------------------------------")
            print(f" [TIME: {int(now)}s] ACTIVE PHASE: {current_phase_name}")
            with signals_lock:
                print(f" SIGNALS:  North={signals[0]}  South={signals[1]}  East={signals[2]}  West={signals[3]}")
            print(f" QUEUES:   North: {lanes[0].count()} cars | South: {lanes[1].count()} cars | East: {lanes[2].count()} cars | West: {lanes[3].count()} cars")
            print(f" STATS:    Total Served: {sum(l.total_served for l in lanes)} | Starvation Violations: {starvation_count} | Watchdog: {'SAFE FALLBACK' if is_fallback_active else 'HEALTHY'}")

    except KeyboardInterrupt:
        print("\n[SHUTDOWN] Stopping simulation gracefully...")

if __name__ == "__main__":
    main()
