// Smart Traffic Simulation - Frontend Controller & Visualizer
(function() {
  'use strict';

  // State
  let currentSnapshot = null;
  let pollInterval = null;
  let chartWait = null;
  let chartLatency = null;
  let chartThroughput = null;
  let chartFairness = null;

  const waitHistory = [];
  const latencyHistory = [];
  const maxHistoryPoints = 25;

  // Canvas context
  const canvas = document.getElementById('trafficCanvas');
  const ctx = canvas.getContext('2d');
  const CW = canvas.width;
  const CH = canvas.height;
  const CX = CW / 2;
  const CY = CH / 2;
  const ROAD_W = 160;
  const HALF_ROAD = ROAD_W / 2;

  // DOM elements
  const simStateBadge = document.getElementById('simStateBadge');
  const phaseBadge = document.getElementById('phaseBadge');
  const safetyBadge = document.getElementById('safetyBadge');
  const fallbackBadge = document.getElementById('fallbackBadge');

  // Initialize
  window.addEventListener('DOMContentLoaded', () => {
    initTabs();
    initControls();
    initCharts();
    startPolling();
    requestAnimationFrame(renderLoop);
  });

  // Tab Switching
  function initTabs() {
    const tabs = document.querySelectorAll('.nav-tab');
    tabs.forEach(tab => {
      tab.addEventListener('click', () => {
        tabs.forEach(t => t.classList.remove('active'));
        document.querySelectorAll('.tab-pane').forEach(p => p.classList.remove('active'));
        tab.classList.add('active');
        const targetId = tab.getAttribute('data-tab');
        const targetPane = document.getElementById(targetId);
        if (targetPane) targetPane.classList.add('active');
      });
    });
  }

  // Controls Wiring
  function initControls() {
    document.getElementById('btnStart').addEventListener('click', () => {
      apiPost('/api/control', 'action=start');
    });

    document.getElementById('btnPause').addEventListener('click', () => {
      apiPost('/api/control', 'action=stop');
    });

    document.getElementById('btnReset').addEventListener('click', () => {
      apiPost('/api/control', 'action=reset');
      waitHistory.length = 0;
      latencyHistory.length = 0;
    });

    const rngSpeed = document.getElementById('rngSpeed');
    const lblSpeed = document.getElementById('lblSpeed');
    rngSpeed.addEventListener('input', (e) => {
      lblSpeed.textContent = e.target.value + 'x';
      apiPost('/api/control', 'speed=' + e.target.value);
    });

    document.getElementById('selAlgorithm').addEventListener('change', (e) => {
      apiPost('/api/control', 'alg=' + e.target.value);
    });

    document.getElementById('selPreset').addEventListener('change', (e) => {
      const val = e.target.value;
      let rate = 20;
      if (val === 'uniform') rate = 15;
      if (val === 'asymmetric') rate = 35;
      if (val === 'starvation') rate = 45;
      if (val === 'rush') rate = 50;
      apiPost('/api/control', 'rate=' + rate);
    });

    // Emergency dispatch
    document.getElementById('btnDispatchEmergency').addEventListener('click', () => {
      const dir = document.getElementById('selEmergencyDir').value;
      const type = document.getElementById('selEmergencyType').value;
      apiPost('/api/inject_emergency', `dir=${dir}&type=${type}`)
        .then(() => {
          document.getElementById('emergencyStatusLog').textContent =
            `Dispatched ${type.toUpperCase()} on approach ${['NORTH','SOUTH','EAST','WEST'][dir]}! Requesting priority preemption...`;
        });
    });

    // Fault Injection
    document.getElementById('btnInjectFault').addEventListener('click', () => {
      const worker = document.getElementById('selFaultTarget').value;
      const fault = document.getElementById('selFaultType').value;
      apiPost('/api/inject_fault', `worker=${worker}&fault=${fault}`)
        .then(() => {
          document.getElementById('faultStatusLog').textContent =
            `⚠️ Injected fault type ${fault} on worker ${worker}. Watchdog will detect heartbeat failure.`;
        });
    });

    document.getElementById('btnRecoverFault').addEventListener('click', () => {
      apiPost('/api/recover', '')
        .then(() => {
          document.getElementById('faultStatusLog').textContent =
            `✓ Recovery triggered. Normal scheduling restored.`;
        });
    });

    // Race Demo Runner
    document.getElementById('btnRunRaceDemo').addEventListener('click', runRaceDemo);

    // Batch Benchmark Runner
    document.getElementById('btnRunBatchBenchmark').addEventListener('click', runBatchBenchmarks);
  }

  // API Helpers
  function apiPost(url, body) {
    return fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: body
    }).then(res => res.json()).catch(err => console.error(err));
  }

  function apiGet(url) {
    return fetch(url).then(res => res.json()).catch(err => console.error(err));
  }

  // Polling loop
  function startPolling() {
    pollInterval = setInterval(() => {
      apiGet('/api/status').then(snap => {
        if (!snap) return;
        currentSnapshot = snap;
        updateUI(snap);
      });
    }, 120);
  }

  // Update DOM Telemetry
  function updateUI(snap) {
    // Badges
    simStateBadge.textContent = snap.isRunning ? 'RUNNING' : 'STOPPED';
    simStateBadge.className = 'badge ' + (snap.isRunning ? 'badge-running' : 'badge-stopped');

    const pName = snap.currentPhaseName || 'UNKNOWN';
    const pRemain = Math.max(0, snap.phaseDurationSec - snap.phaseElapsedSec).toFixed(1);
    phaseBadge.textContent = `PHASE: ${pName} (${pRemain}s)`;

    if (snap.safetyConflictDetected) {
      safetyBadge.textContent = '⚠️ SAFETY CONFLICT DETECTED!';
      safetyBadge.className = 'badge badge-conflict';
    } else {
      safetyBadge.textContent = '✓ CONFLICT-FREE';
      safetyBadge.className = 'badge badge-safe';
    }

    if (snap.isSafeFallbackActive) {
      fallbackBadge.textContent = 'SAFE FALLBACK ACTIVE';
      fallbackBadge.className = 'badge badge-fallback';
    } else {
      fallbackBadge.textContent = 'NORMAL MODE';
      fallbackBadge.className = 'badge badge-normal';
    }

    // Lane cards
    if (snap.lanes && snap.lanes.length >= 4) {
      const dirs = ['N', 'S', 'E', 'W'];
      for (let i = 0; i < 4; ++i) {
        const l = snap.lanes[i];
        const d = dirs[i];
        document.getElementById(`valQueue${d}`).textContent = l.queueLength;
        document.getElementById(`valWait${d}`).textContent = l.avgWaitTimeSec.toFixed(1) + 's';
        document.getElementById(`valMaxWait${d}`).textContent = l.maxWaitTimeSec.toFixed(1) + 's';

        const sig = snap.signals[i] || 'RED';
        const pill = document.getElementById(`sigBadge${d}`);
        pill.textContent = sig;
        pill.className = 'sig-pill sig-' + sig.toLowerCase();
      }
    }

    // OS Metrics
    document.getElementById('osCpuVal').textContent = snap.cpuUtilizationPercent.toFixed(1);
    document.getElementById('osMemVal').textContent = snap.memoryUsageMb.toFixed(1);
    document.getElementById('osLatencyVal').textContent = snap.controllerDecisionLatencyMicrosec.toFixed(0);
    document.getElementById('osSyncVal').textContent = snap.avgSyncOverheadMicrosec.toFixed(1);
    document.getElementById('osDeadlinesVal').textContent = snap.deadlineMissCount;
    document.getElementById('osStarvationVal').textContent = snap.starvationViolationsCount;

    // Threads Table
    if (snap.threads) {
      const tbody = document.getElementById('threadsTableBody');
      tbody.innerHTML = '';
      snap.threads.forEach(t => {
        const row = document.createElement('tr');
        const age = (snap.simTime - t.lastHeartbeatSec).toFixed(2);
        const statusColor = t.status === 'RUNNING' ? '#34d399' : (t.status === 'FAILED' ? '#f87171' : '#fbbf24');
        const healthBadge = t.isHealthy
          ? '<span style="color:#34d399; font-weight:700;">✓ Healthy</span>'
          : '<span style="color:#f87171; font-weight:700;">⚠ MISSED HB</span>';

        row.innerHTML = `
          <td><b>${t.name}</b></td>
          <td><code>${t.osThreadId || 'N/A'}</code></td>
          <td><span style="color:${statusColor}; font-weight:600;">${t.status}</span></td>
          <td>${age}s ago</td>
          <td>${t.lockWaitCount}</td>
          <td>${t.avgLockWaitMicrosec.toFixed(1)} µs</td>
          <td>${healthBadge}</td>
        `;
        tbody.appendChild(row);
      });
    }

    // Update real-time line charts
    waitHistory.push(snap.overallAvgWaitSec);
    latencyHistory.push(snap.controllerDecisionLatencyMicrosec);
    if (waitHistory.length > maxHistoryPoints) waitHistory.shift();
    if (latencyHistory.length > maxHistoryPoints) latencyHistory.shift();

    if (chartWait) {
      chartWait.data.labels = waitHistory.map((_, i) => `${i}`);
      chartWait.data.datasets[0].data = [...waitHistory];
      chartWait.update('none');
    }
    if (chartLatency) {
      chartLatency.data.labels = latencyHistory.map((_, i) => `${i}`);
      chartLatency.data.datasets[0].data = [...latencyHistory];
      chartLatency.update('none');
    }

    // Watchdog log box
    if (snap.activeFault && snap.activeFault > 0) {
      const wBox = document.getElementById('watchdogLogBox');
      const timeStr = new Date().toLocaleTimeString();
      if (!wBox.innerHTML.includes(`[${snap.activeFaultName}]`)) {
        wBox.innerHTML += `<p style="color:#f87171;">[${timeStr}] WATCHDOG ALERT: Injected fault detected: ${snap.activeFaultName}. Triggering recovery dispatch.</p>`;
        wBox.scrollTop = wBox.scrollHeight;
      }
    }
  }

  // Chart.js Initializer
  function initCharts() {
    const ctxW = document.getElementById('chartWaitTime').getContext('2d');
    chartWait = new Chart(ctxW, {
      type: 'line',
      data: {
        labels: [],
        datasets: [{
          label: 'Avg Wait Time (s)',
          data: [],
          borderColor: '#38bdf8',
          backgroundColor: 'rgba(56, 189, 248, 0.1)',
          fill: true,
          tension: 0.3
        }]
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        scales: {
          x: { display: false },
          y: { grid: { color: '#1e293b' }, ticks: { color: '#94a3b8' } }
        },
        plugins: { legend: { labels: { color: '#cbd5e1' } } }
      }
    });

    const ctxL = document.getElementById('chartLatency').getContext('2d');
    chartLatency = new Chart(ctxL, {
      type: 'line',
      data: {
        labels: [],
        datasets: [{
          label: 'Decision Latency (µs)',
          data: [],
          borderColor: '#f59e0b',
          backgroundColor: 'rgba(245, 158, 11, 0.1)',
          fill: true,
          tension: 0.3
        }]
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        scales: {
          x: { display: false },
          y: { grid: { color: '#1e293b' }, ticks: { color: '#94a3b8' } }
        },
        plugins: { legend: { labels: { color: '#cbd5e1' } } }
      }
    });

    const ctxT = document.getElementById('chartThroughputBar').getContext('2d');
    chartThroughput = new Chart(ctxT, {
      type: 'bar',
      data: {
        labels: ['Baseline A (Fixed)', 'Baseline B (Adaptive)', 'Proposed C (Starvation-Resistant)'],
        datasets: [{
          label: 'Throughput (Vehicles/Min)',
          data: [42, 54, 61],
          backgroundColor: ['#64748b', '#3b82f6', '#10b981']
        }]
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        scales: {
          x: { ticks: { color: '#cbd5e1' } },
          y: { grid: { color: '#1e293b' }, ticks: { color: '#94a3b8' } }
        },
        plugins: { legend: { display: false } }
      }
    });

    const ctxF = document.getElementById('chartFairnessBar').getContext('2d');
    chartFairness = new Chart(ctxF, {
      type: 'bar',
      data: {
        labels: ['Baseline A', 'Baseline B', 'Proposed C'],
        datasets: [
          {
            label: "Jain's Fairness (0-1)",
            data: [0.72, 0.65, 0.94],
            backgroundColor: '#8b5cf6'
          },
          {
            label: 'Starvation Events Count',
            data: [14, 22, 0],
            backgroundColor: '#ef4444'
          }
        ]
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        scales: {
          x: { ticks: { color: '#cbd5e1' } },
          y: { grid: { color: '#1e293b' }, ticks: { color: '#94a3b8' } }
        },
        plugins: { legend: { labels: { color: '#cbd5e1' } } }
      }
    });
  }

  // Race Demo
  function runRaceDemo() {
    const sync = document.getElementById('selRaceMode').value;
    const threads = document.getElementById('selRaceThreads').value;
    const iters = document.getElementById('selRaceIters').value;

    fetch('/api/race_demo', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: `sync=${sync}&threads=${threads}&iters=${iters}`
    })
    .then(r => r.json())
    .then(res => {
      document.getElementById('raceResultsBox').style.display = 'block';
      document.getElementById('raceExpectedVal').textContent = res.expectedTotal.toLocaleString();
      document.getElementById('raceActualVal').textContent = res.actualTotal.toLocaleString();
      document.getElementById('raceLostVal').textContent = res.lostUpdates.toLocaleString();
      document.getElementById('raceCorruptionVal').textContent = res.corruptionRatePercent.toFixed(2) + '%';
      document.getElementById('raceDurationVal').textContent = res.executionDurationMs.toFixed(2) + ' ms';
      document.getElementById('raceAnalysisText').textContent = res.analysisSummary;
    });
  }

  // Automated Batch Benchmark
  function runBatchBenchmarks() {
    const btn = document.getElementById('btnRunBatchBenchmark');
    btn.textContent = '⏳ Running Multi-Scenario Benchmarks...';
    btn.disabled = true;

    fetch('/api/run_benchmarks', { method: 'POST' })
      .then(r => r.json())
      .then(data => {
        btn.textContent = '⚡ Run All Benchmarks';
        btn.disabled = false;
        if (!data || !data.results) return;

        const tbody = document.getElementById('benchmarkTableBody');
        tbody.innerHTML = '';
        data.results.forEach(r => {
          const tr = document.createElement('tr');
          tr.innerHTML = `
            <td><b>${r.scenario}</b></td>
            <td>${r.algorithm}</td>
            <td><code>${r.syncStrategy}</code></td>
            <td>${r.totalServed}</td>
            <td><b>${r.throughputPerMin.toFixed(1)}</b></td>
            <td>${r.avgWaitSec.toFixed(1)}s</td>
            <td>${r.maxWaitSec.toFixed(1)}s</td>
            <td><b>${r.jainsFairnessIndex.toFixed(4)}</b></td>
            <td style="color:${r.starvationEvents > 0 ? '#f87171' : '#34d399'}; font-weight:700;">${r.starvationEvents}</td>
            <td>${r.avgCpuPercent.toFixed(1)}%</td>
            <td>${r.avgControllerLatencyMicrosec.toFixed(0)} µs</td>
            <td>${r.avgSyncOverheadMicrosec.toFixed(1)} µs</td>
          `;
          tbody.appendChild(tr);
        });

        // Update charts with summary
        const cResults = data.results.filter(x => x.scenario.includes('Asymmetric'));
        if (cResults.length >= 3) {
          chartThroughput.data.datasets[0].data = cResults.map(x => x.throughputPerMin);
          chartThroughput.update();

          chartFairness.data.datasets[0].data = cResults.map(x => x.jainsFairnessIndex);
          chartFairness.data.datasets[1].data = cResults.map(x => x.starvationEvents);
          chartFairness.update();
        }
      })
      .catch(err => {
        btn.textContent = '⚡ Run All Benchmarks';
        btn.disabled = false;
        console.error(err);
      });
  }

  // Canvas Drawing & Animation Loop
  function renderLoop() {
    drawIntersection();
    drawSignals();
    drawVehicles();
    requestAnimationFrame(renderLoop);
  }

  function drawIntersection() {
    // Background grass
    ctx.fillStyle = '#0b1320';
    ctx.fillRect(0, 0, CW, CH);

    // Roads (Asphalt)
    ctx.fillStyle = '#1e2430';
    // North-South Road
    ctx.fillRect(CX - HALF_ROAD, 0, ROAD_W, CH);
    // East-West Road
    ctx.fillRect(0, CY - HALF_ROAD, CW, ROAD_W);

    // Sidewalk curbs
    ctx.strokeStyle = '#334155';
    ctx.lineWidth = 3;
    // Corners
    ctx.strokeRect(0, 0, CX - HALF_ROAD, CY - HALF_ROAD);
    ctx.strokeRect(CX + HALF_ROAD, 0, CX - HALF_ROAD, CY - HALF_ROAD);
    ctx.strokeRect(0, CY + HALF_ROAD, CX - HALF_ROAD, CY - HALF_ROAD);
    ctx.strokeRect(CX + HALF_ROAD, CY + HALF_ROAD, CX - HALF_ROAD, CY - HALF_ROAD);

    // Center yellow double lines
    ctx.strokeStyle = '#eab308';
    ctx.lineWidth = 2;
    ctx.setLineDash([]);

    // North Center
    ctx.beginPath();
    ctx.moveTo(CX - 2, 0); ctx.lineTo(CX - 2, CY - HALF_ROAD);
    ctx.moveTo(CX + 2, 0); ctx.lineTo(CX + 2, CY - HALF_ROAD);
    ctx.stroke();

    // South Center
    ctx.beginPath();
    ctx.moveTo(CX - 2, CY + HALF_ROAD); ctx.lineTo(CX - 2, CH);
    ctx.moveTo(CX + 2, CY + HALF_ROAD); ctx.lineTo(CX + 2, CH);
    ctx.stroke();

    // West Center
    ctx.beginPath();
    ctx.moveTo(0, CY - 2); ctx.lineTo(CX - HALF_ROAD, CY - 2);
    ctx.moveTo(0, CY + 2); ctx.lineTo(CX - HALF_ROAD, CY + 2);
    ctx.stroke();

    // East Center
    ctx.beginPath();
    ctx.moveTo(CX + HALF_ROAD, CY - 2); ctx.lineTo(CW, CY - 2);
    ctx.moveTo(CX + HALF_ROAD, CY + 2); ctx.lineTo(CW, CY + 2);
    ctx.stroke();

    // White Stop lines
    ctx.strokeStyle = '#ffffff';
    ctx.lineWidth = 4;
    // North stop line (southbound traffic)
    ctx.beginPath();
    ctx.moveTo(CX - HALF_ROAD, CY - HALF_ROAD);
    ctx.lineTo(CX, CY - HALF_ROAD);
    ctx.stroke();

    // South stop line (northbound traffic)
    ctx.beginPath();
    ctx.moveTo(CX, CY + HALF_ROAD);
    ctx.lineTo(CX + HALF_ROAD, CY + HALF_ROAD);
    ctx.stroke();

    // East stop line (westbound traffic)
    ctx.beginPath();
    ctx.moveTo(CX + HALF_ROAD, CY - HALF_ROAD);
    ctx.lineTo(CX + HALF_ROAD, CY);
    ctx.stroke();

    // West stop line (eastbound traffic)
    ctx.beginPath();
    ctx.moveTo(CX - HALF_ROAD, CY);
    ctx.lineTo(CX - HALF_ROAD, CY + HALF_ROAD);
    ctx.stroke();

    // Pedestrian Crosswalks (Zebra stripes)
    drawCrosswalk(CX - HALF_ROAD, CY - HALF_ROAD - 24, ROAD_W, 20, true);
    drawCrosswalk(CX - HALF_ROAD, CY + HALF_ROAD + 4, ROAD_W, 20, true);
    drawCrosswalk(CX - HALF_ROAD - 24, CY - HALF_ROAD, 20, ROAD_W, false);
    drawCrosswalk(CX + HALF_ROAD + 4, CY - HALF_ROAD, 20, ROAD_W, false);
  }

  function drawCrosswalk(x, y, w, h, horizontal) {
    ctx.fillStyle = 'rgba(255, 255, 255, 0.4)';
    const stripeCount = 6;
    if (horizontal) {
      const step = w / stripeCount;
      for (let i = 0; i < stripeCount; ++i) {
        ctx.fillRect(x + i * step + 4, y, step - 8, h);
      }
    } else {
      const step = h / stripeCount;
      for (let i = 0; i < stripeCount; ++i) {
        ctx.fillRect(x, y + i * step + 4, w, step - 8);
      }
    }
  }

  function drawSignals() {
    if (!currentSnapshot || !currentSnapshot.signals) return;
    const signals = currentSnapshot.signals;

    // Traffic signal housings
    drawSignalHead(CX - HALF_ROAD - 22, CY - HALF_ROAD - 40, signals[0]); // North
    drawSignalHead(CX + HALF_ROAD + 8, CY + HALF_ROAD + 8, signals[1]);   // South
    drawSignalHead(CX + HALF_ROAD + 8, CY - HALF_ROAD - 40, signals[2]);  // East
    drawSignalHead(CX - HALF_ROAD - 22, CY + HALF_ROAD + 8, signals[3]);  // West
  }

  function drawSignalHead(x, y, colorStr) {
    ctx.fillStyle = '#0f172a';
    ctx.strokeStyle = '#475569';
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.roundRect(x, y, 16, 36, 4);
    ctx.fill();
    ctx.stroke();

    const redOn = colorStr === 'RED';
    const yelOn = colorStr === 'YELLOW';
    const grnOn = colorStr === 'GREEN';

    drawLed(x + 8, y + 6, redOn ? '#ef4444' : '#450a0a', redOn);
    drawLed(x + 8, y + 18, yelOn ? '#f59e0b' : '#451a03', yelOn);
    drawLed(x + 8, y + 30, grnOn ? '#10b981' : '#022c22', grnOn);
  }

  function drawLed(x, y, color, glowing) {
    ctx.beginPath();
    ctx.arc(x, y, 4, 0, Math.PI * 2);
    ctx.fillStyle = color;
    if (glowing) {
      ctx.shadowColor = color;
      ctx.shadowBlur = 8;
    } else {
      ctx.shadowBlur = 0;
    }
    ctx.fill();
    ctx.shadowBlur = 0; // reset
  }

  function drawVehicles() {
    if (!currentSnapshot || !currentSnapshot.lanes) return;

    // Draw vehicles queued in each direction
    const lanes = currentSnapshot.lanes;
    const signals = currentSnapshot.signals || ['RED','RED','RED','RED'];

    // Lane 0: North approach (entering from North going South)
    drawLaneVehicles(lanes[0], signals[0], 'NORTH');
    // Lane 1: South approach (entering from South going North)
    drawLaneVehicles(lanes[1], signals[1], 'SOUTH');
    // Lane 2: East approach (entering from East going West)
    drawLaneVehicles(lanes[2], signals[2], 'EAST');
    // Lane 3: West approach (entering from West going East)
    drawLaneVehicles(lanes[3], signals[3], 'WEST');
  }

  function drawLaneVehicles(lane, signal, dir) {
    const qLen = Math.min(lane.queueLength, 8);
    const isGreen = signal === 'GREEN';

    for (let i = 0; i < qLen; ++i) {
      const isLead = (i === 0);
      const isEmerg = (i === 0 && lane.hasEmergency);
      const offset = i * 36;

      let vx, vy, vw, vh;
      if (dir === 'NORTH') {
        vx = CX - HALF_ROAD / 2 - 12;
        vy = (CY - HALF_ROAD - 30) - offset;
        vw = 24;
        vh = 30;
      } else if (dir === 'SOUTH') {
        vx = CX + HALF_ROAD / 2 - 12;
        vy = (CY + HALF_ROAD + 10) + offset;
        vw = 24;
        vh = 30;
      } else if (dir === 'EAST') {
        vx = (CX + HALF_ROAD + 10) + offset;
        vy = CY - HALF_ROAD / 2 - 12;
        vw = 30;
        vh = 24;
      } else if (dir === 'WEST') {
        vx = (CX - HALF_ROAD - 30) - offset;
        vy = CY + HALF_ROAD / 2 - 12;
        vw = 30;
        vh = 24;
      }

      // If green and lead vehicle, animate moving through center!
      if (isGreen && isLead) {
        const animOffset = (Date.now() % 1500) / 1500;
        if (dir === 'NORTH') vy += animOffset * 180;
        if (dir === 'SOUTH') vy -= animOffset * 180;
        if (dir === 'EAST') vx -= animOffset * 180;
        if (dir === 'WEST') vx += animOffset * 180;
      }

      drawCar(vx, vy, vw, vh, dir, isEmerg);
    }
  }

  function drawCar(x, y, w, h, dir, isEmergency) {
    ctx.save();
    ctx.beginPath();
    ctx.roundRect(x, y, w, h, 4);

    if (isEmergency) {
      // Emergency flashing siren
      const flash = (Date.now() % 400) > 200;
      ctx.fillStyle = flash ? '#ef4444' : '#ffffff';
      ctx.shadowColor = '#ef4444';
      ctx.shadowBlur = 10;
    } else {
      ctx.fillStyle = '#38bdf8';
      ctx.shadowBlur = 0;
    }
    ctx.fill();

    // Windshield
    ctx.fillStyle = '#0f172a';
    if (w < h) {
      // Vertical
      ctx.fillRect(x + 3, y + (dir === 'NORTH' ? 18 : 6), w - 6, 6);
    } else {
      // Horizontal
      ctx.fillRect(x + (dir === 'EAST' ? 6 : 18), y + 3, 6, h - 6);
    }

    ctx.restore();
  }

})();
