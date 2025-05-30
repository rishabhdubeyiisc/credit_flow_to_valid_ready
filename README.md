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
## 1.  Modules (all in `src/main.cpp`)

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

```
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

Use the CSV in spreadsheets or feed the whole README back to an LLM for further design space exploration.

---
## 6.  Typical experiments

1. **Bandwidth-delay product** – raise `TX_FIFO_DEPTH` while adjusting `NOC_STATIC_LATENCY_ONE_WAY` to observe buffer requirements.
2. **Congestion** – disable popping in `iEP_after_RX` to see credit starvation propagate back through the network.
3. **Thread scaling** – widen `RawTLP.thread_id`, update loop bounds, and observe how credit bus width vs window sizing changes buffer requirements.
4. **Stall pattern tuning** – adjust `NOC_STALL_PCT` and `NOC_PATTERN_LEN` to observe impact on throughput and latency.

---
## 7. Implementation Details

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
## 8. Design Considerations

### Buffer Sizing
1. **TX FIFO**
   - Must cover network latency (NOC_STATIC_LATENCY_ONE_WAY)
   - Should handle burst traffic during stall periods
   - Current size: 16 entries

2. **RX FIFO**
   - Provides elasticity after network
   - Smaller size (2 entries) due to credit-based flow control
   - Prevents credit starvation

3. **Thread Queues**
   - Fixed depth (8 entries) per thread
   - Must handle credit round-trip latency
   - Balances throughput and resource usage

### Network Parameters
1. **Latency**
   - Fixed one-way latency: 150 cycles
   - Accounts for C2C and GPU fabric delays
   - Includes SMN bridge overhead

2. **Stall Pattern**
   - 15% stall duty cycle
   - 100-cycle pattern length
   - Deterministic for predictable behavior

### Performance Metrics
1. **Throughput**
   - Measured in packets per second
   - Affected by:
     * Network latency
     * Stall percentage
     * Buffer depths
     * Credit window size

2. **Latency**
   - End-to-end packet delay
   - Components:
     * Network traversal time
     * Buffer queuing time
     * Credit round-trip time

---
## 9. Debugging Guide

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
## 10. Future Enhancements

### Potential Improvements
1. **Dynamic Buffer Sizing**
   - Adaptive TX/RX FIFO depths
   - Based on traffic patterns
   - Automatic credit window tuning

2. **Advanced Flow Control**
   - Priority-based scheduling
   - Quality of Service (QoS) support
   - Bandwidth reservation

3. **Network Features**
   - Multiple virtual channels
   - Adaptive routing
   - Error detection/correction

4. **Monitoring**
   - Real-time performance metrics
   - Automatic bottleneck detection
   - Predictive congestion avoidance

### Research Directions
1. **Credit System Optimization**
   - Optimal credit window sizing
   - Credit prediction algorithms
   - Dynamic credit allocation

2. **Buffer Management**
   - Optimal buffer sizing
   - Buffer sharing strategies
   - Memory efficiency

3. **Network Architecture**
   - Topology optimization
   - Routing algorithms
   - Load balancing
