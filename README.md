# SystemC-based Network-on-Chip (NoC) Simulation

This project implements a SystemC-based simulation of a Network-on-Chip (NoC) system with credit-based flow control. The system models a communication path between a Root Complex (RC) and an Endpoint (EP) with intermediate buffering and network elements.

## Table of Contents
1. [Project Overview](#1-project-overview)
2. [System Architecture](#2-system-architecture)
3. [Project Structure](#3-project-structure)
4. [Build & Run](#4-build--run)
5. [Debugging](#5-debugging)
6. [Performance Analysis](#6-performance-analysis)
7. [Development Guide](#7-development-guide)

## 1. Project Overview

The project simulates two communication topologies:

1. **Direct Credit Path**: Direct connection between RC and EP with immediate credit return
2. **Hybrid Path**: Network-based connection with TX/RX FIFOs and credit flow control

### Key Features
- Multi-threaded packet generation
- Credit-based flow control
- Configurable network latency and backpressure
- FIFO buffering with tunable depths
- Comprehensive performance analysis tools

## 2. System Architecture

### Topology Diagrams

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

### Module Hierarchy
```
System
├── Direct Path
│   ├── iRC (Root Complex)
│   └── iEP (Endpoint)
└── Network Path
    ├── iRC_tx
    ├── SimpleTxFIFO
    ├── AxiNoC
    ├── SimpleRxFIFO
    └── iEP_after_RX
```

### Data Flow
```
Packet Flow:
RC → RawTLP → TX FIFO → AXI → NoC → AXI → RX FIFO → RawTLP → EP

Credit Flow:
EP → Credit → CreditTx → AXI → NoC → AXI → CreditRx → Credit → RC
```

## 3. Project Structure

### Directory Layout
```
.
├── src/                    # Source files
│   ├── config.h           # Build-time configuration
│   ├── main.cpp           # Main simulation entry
│   ├── modules.h          # Module declarations
│   ├── modules.cpp        # Module implementations
│   └── payloads.h         # Packet and data structures
├── scripts/               # Analysis and tuning tools
│   ├── analyze_logs.py    # Log analysis
│   ├── fifo_tuner.py      # FIFO depth optimization
│   ├── noc_tuner.py       # NoC parameter tuning
│   └── sanity_ready_loss.py # Ready signal analysis
├── waves_reports/         # Waveform and analysis outputs
├── Makefile              # Build configuration
└── README.md             # This file
```

### Key Files
| File | Purpose |
|------|---------|
| `src/config.h` | Configuration parameters and constants |
| `src/main.cpp` | Simulation setup and module instantiation |
| `src/modules.h` | Module interfaces and declarations |
| `src/modules.cpp` | Module implementations |
| `src/payloads.h` | Data structures and packet formats |

## 4. Build & Run

### Prerequisites
- SystemC 2.3.3 or later
- C++17 compatible compiler
- Python 3.6+ (for analysis scripts)
- GTKWave (for waveform viewing)

### Build Steps
```bash
# Clone the repository
git clone <repository-url>
cd <project-directory>

# Build the project
make clean  # Clean previous build
make        # Build the simulation

# Run the simulation
./build/sim
```

### Output Files
- `irc_iep_flow.vcd`: Direct path waveforms
- `irc_tx_flow.vcd`: Network path waveforms
- `auto_run.log`: Simulation console output
- `fifo_sweep_report.csv`: FIFO tuning results
- `noc_sweep_report.csv`: NoC parameter tuning results

## 5. Debugging

### Waveform Analysis
```bash
# View direct path waveforms
gtkwave irc_iep_flow.vcd

# View network path waveforms
gtkwave irc_tx_flow.vcd
```

### Key Signals to Monitor
1. **Data Path**
   - `valid/ready` handshakes
   - Packet sequence numbers
   - FIFO occupancy

2. **Credit Path**
   - Credit pulses
   - Credit counters
   - Credit bus activity

### Common Issues
1. **Credit Starvation**
   - Check credit window size
   - Monitor credit counter values
   - Verify credit return path

2. **Buffer Overflow**
   - Monitor FIFO occupancy
   - Check buffer depths
   - Verify backpressure signals

3. **Network Congestion**
   - Check NoC latency settings
   - Monitor stall patterns
   - Verify buffer sizes

## 6. Performance Analysis

### Analysis Tools
1. **Log Analysis**
```bash
python3 scripts/analyze_logs.py sim_log.txt
```
Outputs:
- Packet counts
- Average latency
- Throughput metrics
- FIFO occupancy

2. **FIFO Tuning**
```bash
python3 scripts/fifo_tuner.py
```
Sweeps TX/RX FIFO depths and generates performance report.

3. **NoC Parameter Tuning**
```bash
python3 scripts/noc_tuner.py
```
Sweeps through NOC latency and stall percentage combinations to find optimal parameters. The script:
- Tests combinations of latencies [1, 20, 40, 60, 80] and stall percentages [25, 15, 5, 1]
- Verifies that the RXTX path achieves at least 50% of the Direct path's bandwidth
- Generates a CSV report with bandwidth measurements in MB/s
- Uses the same simulation run to compare both topologies (Direct and RXTX)

Usage:
```bash
python3 scripts/noc_tuner.py
```

### Performance Metrics
1. **Throughput**
   - Packets per second
   - Bandwidth (MB/s)
   - Credit utilization

2. **Latency**
   - End-to-end delay
   - Network traversal time
   - Credit return time

3. **Resource Usage**
   - Buffer occupancy
   - Credit bus activity
   - Network utilization

## 7. Development Guide

### Adding New Features
1. **Module Development**
   - Add interface in `modules.h`
   - Implement in `modules.cpp`
   - Update `main.cpp` for instantiation

2. **Configuration Changes**
   - Modify parameters in `config.h`
   - Update build system if needed
   - Add new analysis tools if required

### Testing
1. **Unit Testing**
   - Test individual modules
   - Verify interfaces
   - Check timing constraints

2. **Integration Testing**
   - Verify module interactions
   - Check system behavior
   - Validate performance

### Best Practices
1. **Code Style**
   - Follow SystemC coding guidelines
   - Use consistent naming conventions
   - Document interfaces and parameters

2. **Performance**
   - Monitor resource usage
   - Optimize critical paths
   - Profile system behavior

3. **Debugging**
   - Use waveform analysis
   - Monitor key signals
   - Check error conditions

### Future Work
1. **Performance Optimization**
   - Dynamic buffer sizing
   - Adaptive credit window
   - Priority scheduling

2. **Feature Enhancements**
   - Multiple virtual channels
   - Error detection/correction
   - QoS support

3. **Analysis Tools**
   - Real-time monitoring
   - Bottleneck detection
   - Congestion prediction
