# SystemC-based Network-on-Chip (NoC) Simulation

This project implements a SystemC-based simulation of a Network-on-Chip (NoC) system with credit-based flow control. The system models a communication path between a Root Complex (RC) and an Endpoint (EP) with intermediate buffering and network elements.

```
Direct credit path                            Hybrid (credit + ready/valid)
───────────────────────                      ───────────────────────────────────────────────────────────────
  iRC              credits[2:0]                iRC_tx          credits[2:0]
   │  Raw-valid/TLP    ▲                        │   Raw-valid/TLP   ▲
   ▼                   │                        ▼                  │
  iEP  ◀───────────────┘ (1-cycle loop)       SimpleTxFIFO ─valid/ready─► SimpleRxFIFO ─Raw-valid/TLP─► iEP_after_RX
                                                 ▲                                 ▲                         ▲
                                                 └──── credit bus via ProxyCreditGen ────────────────────────┘
```

Both topologies share one clock and run for 400 µs; you can watch every wave in **GTKW** and mine every statistic with the included Python tools.

---
## 1.  Modules (all in `src/`)

| Module            | Purpose / Notes |
|-------------------|-----------------|
| `iRC` / `iRC_tx`  | Round-robin multi-thread sender. One credit counter per thread; prints `Send seq=n tid=t`. |
| `Threaded_Queue`  | 8-deep FIFO + credit logic for **one** thread. Building block for iEP & front-ends. |
| `ThreadedFrontEnd`| Router + **3×** `Threaded_Queue` + credit OR-combine. Re-used by both iEP flavours. |
| `SimpleTxFIFO`    | *Single* FIFO that converts Raw-valid/TLP into ready/valid. Depth tunable (default 16). |
| `SimpleRxFIFO`    | Reverse converter (ready/valid → Raw-valid/TLP). Depth tunable (default 2). |
| `CreditTx`        | Converts credit pulses to AXI packets for network transmission. |
| `CreditRx`        | Converts AXI credit packets back to credit pulses. |
| `AxiNoC`          | Pipeline with configurable latency that implements deterministic stall patterns. |
| `iEP`             | Classical credit consumer. Pops one pkt/queue every 4 clks. |
| `iEP_after_RX`    | Same as iEP but fed through TX/RX path. |

---
## 2.  Key knobs

| Symbol                                | Description | Default |
|---------------------------------------|-------------|---------|
| `GLOBAL_SENSE_WINDOW`                 | Statistics window for credit accumulation. | 8 |
| `TX_FIFO_DEPTH`                       | Burst buffer at TX; should cover network latency. | 16 |
| `RX_FIFO_DEPTH`                       | Elasticity buffer after network. | 2 |
| `NOC_STATIC_LATENCY_ONE_WAY`          | Fixed pipeline latency through NoC. | 150 |
| `NOC_STALL_PCT`                       | Duty-cycle of ready-low in `AxiNoC` (0-99 %). | 15 % |
| `NOC_PATTERN_LEN`                     | Resolution of stall pattern. | 100 |
| `THREAD_Q_DEPTH`                      | Per-thread depth in iEPs. | 8 |
| `g_enable_popping`                    | Runtime switch to pause the popper. | true |

---
## 3.  Build & run
```bash
make            # -> build/sim
./build/sim     # generates irc_iep_flow.vcd  +  irc_tx_flow.vcd
```
Open either file in **GTKWave** to inspect both data & credit buses.

---
## 4.  Post-run analysis

### Log Analysis
```bash
python3 scripts/analyze_logs.py sim_log.txt
```
prints packet count, average latency and throughput **per topology** by parsing the console log.

---
## 5.  FIFO depth sweeper

### `fifo_tuner.py` (auto-explorer)

1. Iterates over `TX_DEPTHS × RX_DEPTHS` (config arrays inside the script).
2. For every pair it:
   • patches `TX_FIFO_DEPTH` / `RX_FIFO_DEPTH` in `src/main.cpp`
   • `make` + simulation (console captured to `auto_run.log`)
   • runs `analyze_logs.py` and grabs:
     – credit & ready throughput / latency / packet count
     – max FIFO occupancy (TX, RX)
     – duty-cycle of both credit buses
3. Writes a one-line status to the console and appends a full row to
   `fifo_sweep_report.csv` with all metrics.

CSV columns:

| col | meaning |
|-----|---------|
| tx_depth, rx_depth | FIFO sizes under test |
| credit_mpps / ready_mpps | sustained throughput |
| credit_lat_ns / ready_lat_ns | average end-to-end latency |
| credit_pkts / ready_pkts | packets delivered |
| ratio | ready / credit throughput |
| max_tx / max_rx | worst-case FIFO depth seen |
| duty_direct_% / duty_hybrid_% | credit-bus duty cycle seen by monitor |
| status | `OK` if ratio ≥ 0.9 (configurable) |

Run it:

```bash
python3 scripts/fifo_tuner.py | tee fifo_tuner_output.txt
cat fifo_sweep_report.csv | column -t -s,
```
Automatically explores TX/RX FIFO depth combinations and generates a CSV report with performance metrics.

### Ready Signal Analysis
```bash
python3 scripts/sanity_ready_loss.py
```
Analyzes ready signal behavior and potential packet loss scenarios.

---
## 5.  Performance Analysis

The project includes several tools for performance analysis:

1. **Waveform Analysis**
   - GTKWave visualization of both direct and network paths
   - Credit bus monitoring
   - FIFO occupancy tracking

2. **Statistical Analysis**
   - Throughput measurements
   - Latency analysis
   - Credit utilization metrics

3. **Buffer Optimization**
   - Automated FIFO depth tuning
   - Performance vs. resource trade-offs
   - CSV-based reporting

---
## 6.  Implementation Details

### Packet Flow
1. **RC to EP Direct Path**
   - RawTLP packets flow directly with credit-based flow control
   - Credits are returned immediately (1-cycle loop)
   - No network latency or backpressure

2. **RC to EP Through Network**
   - RawTLP → AXI conversion at TX
   - Network traversal with latency and backpressure
   - AXI → RawTLP conversion at RX
   - Credits travel through separate network path

### Credit System
1. **Credit Generation**
   - Each thread queue generates credits independently
   - Credits accumulate over GLOBAL_SENSE_WINDOW cycles
   - Credits are converted to AXI packets for network transmission

2. **Credit Consumption**
   - RC maintains per-thread credit counters
   - Round-robin scheduling across threads
   - Credits consumed on packet transmission

### NoC Implementation
1. **Pipeline Structure**
   - Fixed latency pipeline (NOC_STATIC_LATENCY_ONE_WAY stages)
   - Each stage can hold one AXI word
   - Non-blocking pipeline shifts

2. **Backpressure Mechanism**
   - Predicts stall conditions one cycle ahead
   - Deterministic stall pattern based on NOC_STALL_PCT
   - Prevents packet drops by looking ahead

---
## 7.  Debugging Guide

### Common Issues
1. **Credit Starvation**
   - Symptoms: Low throughput, high latency
   - Check: Credit window size, buffer depths
   - Fix: Increase GLOBAL_SENSE_WINDOW or buffer sizes

2. **Buffer Overflow**
   - Symptoms: Packet drops, credit mismatch
   - Check: TX/RX FIFO depths
   - Fix: Increase buffer sizes or reduce traffic

3. **Network Congestion**
   - Symptoms: High latency, low throughput
   - Check: NOC_STALL_PCT, pattern length
   - Fix: Adjust stall parameters or increase buffers

### Tracing
1. **VCD Files**
   - `irc_iep_flow.vcd`: Direct path signals
   - `irc_tx_flow.vcd`: Network path signals
   - Key signals to monitor:
     * valid/ready handshakes
     * credit pulses
     * FIFO occupancy

2. **Console Output**
   - Packet sequence numbers
   - Thread IDs
   - FIFO depths
   - Credit statistics

---
## 8.  Future Work

1. **Performance Optimization**
   - Dynamic buffer sizing
   - Adaptive credit window tuning
   - Priority-based scheduling

2. **Feature Enhancements**
   - Multiple virtual channels
   - Error detection/correction
   - Quality of Service (QoS) support

3. **Analysis Tools**
   - Real-time performance monitoring
   - Automated bottleneck detection
   - Predictive congestion avoidance
