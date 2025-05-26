# RC-EP Credit-Based Flow Control

This project implements a SystemC simulation of credit-based flow control between a Root Complex (RC) and Endpoint (EP) in a PCIe-like interface with multiple threads.

## Overview

The simulation models the data flow between an RC (sender) and EP (receiver) using credit-based flow control, where:

- The RC sends Transaction Layer Packets (TLPs) to the EP
- The EP grants credits to the RC for each supported thread (3 threads: 1, 2, 3)
- The RC can only send packets when it has available credits for a specific thread
- Both modules operate on a common system clock for simplified timing analysis

## Architecture

The system consists of two main modules operating on a shared clock domain:

```
+-----------+               +-----------+
|    iRC    |               |    iEP    |
|           |   raw_valid   |           |
|           |-------------->|  +-----+  |
|           |               |  | TQ1 |  |
|           |   raw_tlp     |  +-----+  |
|           |-------------->|           |
|           |   (data +     |  +-----+  |
|           |    thread_id) |  | TQ2 |  |
|           |   credit_out  |  +-----+  |
|           |<--------------|           |
|           |   (pulsed     |  +-----+  |
|           |    per thread)|  | TQ3 |  |
+-----------+               |  +-----+  |
    ^                       +-----------+
    |                            ^
    |                            |
    |_____ system_clk (100MHz) __|
```

Where:
- TQ1, TQ2, TQ3: Threaded_Queue modules for threads 1, 2, and 3
- credit_out: Single bit that pulses for each thread when credits are available

## Module Descriptions

### RawTLP Structure

```cpp
struct RawTLP {
    sc_uint<32>   data;       // 32-bit data field
    sc_uint<2>    thread_id;  // thread identifier (1-3)
};
```

### iRC (Root Complex)

The iRC module acts as a sender of TLPs and includes:

- **Credit Monitor Thread**: Tracks incoming credit pulses for each of the 3 threads
- **Credit Counters**: Maintains separate credit counts for threads 1, 2, and 3
- **Sender Thread**: Implements round-robin arbitration between threads with available credits
- **Packet Generation**: Creates sequential packets with incrementing data values

**Key Features**:
- Round-robin scheduling ensures fair access across all threads
- Credit consumption: exactly one credit is consumed per packet sent
- Event-driven design: responds immediately to credit availability

### iEP (Endpoint)

The iEP module acts as a receiver of TLPs and includes:

- **Receiver Thread**: Processes incoming TLPs and routes them to appropriate thread FIFOs
- **Per-Thread FIFOs**: Separate `sc_fifo<RawTLP>` for each thread (capacity: 8 packets)
- **Credit Generator Thread**: Issues credits when FIFO space is available
- **Smart Credit Management**: Prevents over-issuing credits beyond FIFO capacity
- **Deterministic FIFO Popping**: Configurable consumer behavior with deterministic timing

**Key Features**:
- FIFO capacity of 8 packets per thread
- Credit generation only when actual FIFO space is available
- Prevents credit over-subscription through careful accounting
- Deterministic FIFO popping every 4th cycle when enabled
- Global control for FIFO popping behavior

## Signal Interface

| Signal      | Width | Direction | Description                            |
|-------------|-------|-----------|----------------------------------------|
| raw_valid   | 1     | RC → EP   | Indicates valid TLP data from RC       |
| raw_tlp     | 34    | RC → EP   | TLP data (32-bit data + 2-bit thread)  |
| credit[2:0] | 3     | EP → RC   | 3-bit credit bus (one bit per thread)  |
| system_clk  | 1     | -         | Common 100MHz clock for both modules   |
| reset_n     | 1     | -         | Active-low reset signal                |

## FIFO Popping Control

The simulation includes a global control for FIFO popping behavior:

```cpp
bool g_enable_popping = false;  // Global control for FIFO popping
```

When enabled:
- FIFOs are popped deterministically every 4th cycle
- This provides a consistent 25% popping rate
- Helps in predictable testing and analysis

When disabled:
- FIFOs retain their packets
- Useful for testing credit flow without packet consumption

## Timing Diagrams

The project includes WaveDrom timing diagrams to visualize the credit-based flow control:

### Credit Flow Waveform

The `credit_flow_waveform.json` file contains a WaveDrom timing diagram showing:
- System clock and reset sequence
- Credit bus behavior (showing values 0 and 7 representing different thread combinations)
- Individual credit bits for threads 0, 1, and 2
- Raw valid signal and TLP data flow
- Thread ID patterns in the transmitted packets

To view the timing diagram:
1. Copy the contents of `credit_flow_waveform.json`
2. Paste into [WaveDrom Editor](https://wavedrom.com/editor.html)
3. Observe the credit-based flow control timing relationships

## Building and Running

### Prerequisites

- SystemC 3.0.1 or later
- C++ compiler with C++17 support

### Build Instructions

To build the simulation:

```bash
make
```

To clean and rebuild:

```bash
make clean && make
```

### Running the Simulation

```bash
./build/sim
```

This will execute the simulation and generate a VCD waveform file named `irc_iep_flow.vcd`.

### Complete Workflow

Here's a step-by-step guide to run and analyze the simulation:

1. **Compile the simulation**:
   ```bash
   make clean && make
   ```

2. **Run the simulation**:
   ```bash
   ./build/sim
   ```

3. **View the waveform** (requires GTKWave):
   ```bash
   gtkwave irc_iep_flow.vcd
   ```

4. **Analyze using the analysis tools** (see [Analysis Tools](#analysis-tools) section)

## Simulation Output

The simulation generates multiple outputs:

- **Primary VCD**: `irc_iep_flow.vcd` - Current simulation results
- **Reference VCD**: `waves/irc_iep_flow_tansparent_3threads.vcd` - Reference implementation for comparison
- **Timing Analysis**: WaveDrom-compatible timing diagrams

The simulation:
- Applies a reset for 20ns
- Runs for 20μs to demonstrate sustained credit-based flow control
- Uses a single 100MHz system clock for both RC and EP modules

Key signals to observe:
- `raw_valid` - When data is being transmitted
- `raw_tlp.data` - Sequential packet data values
- `raw_tlp.thread_id` - Thread routing information
- `credit[2:0]` - Multi-bit credit bus from EP to RC

## Analysis Tools

This project includes a set of analysis tools to help understand and compare simulation behavior.

### Available Tools

The tools are located in the `analysis_tools/` directory:

- `analyze_vcd.py`: Python script to parse VCD files and extract key metrics
- `analyze_baseline.sh`: Script to capture baseline behavior for future comparison
- `compare_with_baseline.sh`: Script to compare current behavior against the baseline

### Analyzing Simulation Results

#### Establishing a Baseline

Before making changes to the code, capture a baseline for comparison:

```bash
./analysis_tools/analyze_baseline.sh
```

This will:
1. Build and run the simulation
2. Save the VCD file to the `baseline/` directory
3. Analyze the VCD file and generate metrics
4. Save a summary report to `baseline/irc_iep_flow.vcd.summary.txt`

#### Comparing with Baseline

After making changes to the code, compare the new behavior to the baseline:

```bash
./analysis_tools/compare_with_baseline.sh
```

This will:
1. Build and run the simulation with your changes
2. Save the VCD file to the `current/` directory
3. Analyze the current behavior
4. Compare key metrics between baseline and current runs
5. Display a detailed comparison report

### Key Metrics

The analysis tools extract and compare these important metrics:

1. **Credit Flow Metrics**:
   - Total credit changes: Number of transitions in the credit signal
   - Credit pulse count: Number of non-zero credit values
   - Credit value distribution: Which threads received credits

2. **Data Flow Metrics**:
   - Valid transitions: Number of transitions in the valid signal
   - Valid pulse count: Number of packets sent
   - Thread distribution: Distribution of packets sent per thread

### Interpreting Results

- **Credit changes**: More credit changes indicate more active credit flow control
- **Credit distribution**: Shows which threads are receiving credits (e.g., '111' means all threads)
- **Valid pulses**: Indicates the number of packets successfully transferred
- **Thread distribution**: Shows which threads were used to send packets

When changes affect flow control behavior, you'll typically see differences in:
- Credit pulse patterns
- The ratio of credits to valid pulses
- Thread utilization distribution

## Flow Control Mechanics

1. **Initialization**: EP starts with empty FIFOs, issues initial credits for all threads
2. **Credit Reception**: RC monitors credit pulses and updates per-thread credit counters
3. **Packet Transmission**: RC uses round-robin arbitration among threads with available credits
4. **Credit Consumption**: Each transmitted packet consumes exactly one credit
5. **FIFO Management**: EP tracks FIFO occupancy and generates credits when space becomes available
6. **Backpressure**: When FIFOs fill up, EP stops issuing credits, naturally throttling RC transmission

## Clock Domain Architecture

**Current Implementation**: Single Clock Domain
- Both RC and EP operate on the same `system_clk` (100MHz)
- Simplified timing analysis with no clock domain crossing concerns
- Deterministic behavior with synchronous credit and data flow

**Benefits of Single Clock**:
- Eliminates metastability concerns
- Simplifies debug and timing analysis
- Predictable credit-to-data timing relationships
- Easier verification of credit accounting accuracy

## AI Development Guide

If you are an AI assistant helping with this codebase, here's what you need to know:

### Project Context

This SystemC simulation models a credit-based flow control system similar to what might be used in PCIe interfaces. It demonstrates how data packets can flow between a sending component (RC) and a receiving component (EP) while respecting buffer capacity constraints through a credit mechanism.

### Key Design Principles

1. **Credit-Based Flow Control**: The fundamental principle is that the sender (RC) can only transmit when it has credits, and the receiver (EP) only issues credits when it has buffer space.

2. **Multi-Threading**: The system supports 3 independent threads of communication, each with its own credit accounting and FIFO buffering.

3. **Single Clock Domain**: Both modules operate on a common system clock, eliminating clock domain crossing complexity.

4. **SystemC Methodology**: The code follows standard SystemC practices with modules, ports, signals, and processes.

### Current Architecture Features

1. **Round-Robin Arbitration**: The RC implements fair scheduling across threads with available credits
2. **Event-Driven Credit Monitoring**: Immediate response to credit availability
3. **Smart Credit Management**: EP prevents over-issuing credits beyond actual FIFO capacity
4. **Standard C++ Collections**: Uses `std::queue` for FIFO implementation

### Common Modifications

When working with this codebase, typical modifications might include:

1. **Changing the TLP structure**: You might need to expand the RawTLP to include more fields for different protocols.

2. **Adjusting FIFO depths**: The FIFO_CAPACITY constant (currently 8) controls buffer depth and affects system throughput.

3. **Adding performance counters**: To analyze throughput, latency, or buffer utilization.

4. **Different arbitration policies**: The RC currently uses round-robin; could be modified for priority-based or weighted fair queuing.

5. **Credit allocation strategies**: Modify how EP decides which threads get credits.

### SystemC-Specific Considerations

When modifying SystemC code:

1. **Signal drivers**: Each signal must have exactly one driver (one SC_OUT or SC_INOUT port connected).

2. **Event notifications**: Be careful with event notifications to avoid race conditions.

3. **SC_THREAD vs SC_METHOD**: Understand when to use each (SC_THREAD can suspend execution with wait(), SC_METHOD cannot).

4. **Sensitivity lists**: Keep them accurate to ensure processes are triggered appropriately.

### Common Issues

1. **Multiple driver errors**: Ensure signals are only driven by one output port.

2. **Deadlocks**: Can occur if credit flow is impeded or circular dependencies exist.

3. **Elaboration errors**: Usually caused by improper port binding or initialization.

4. **Delta cycle loops**: Watch for processes that keep activating each other without advancing simulation time.

When assisting with this code, prioritize maintaining the credit-based flow control semantics while allowing for extensions to support more complex protocols or system configurations.

### WaveDrom Integration

The project includes timing diagram visualization using WaveDrom format:

- **File**: `credit_flow_waveform.json`
- **Format**: JSON describing signal waveforms
- **Usage**: Copy content to WaveDrom editor for visualization
- **Signals**: Includes system clock, reset, credit bus, and data flow signals

### Exploring the Codebase and Project Evolution

If you're an AI assistant helping with this project, here are suggestions for exploring the codebase and understanding its evolution:

#### File Structure and Reading the Code

When working with this codebase:

1. **Start with main.cpp**: This contains the complete simulation setup including module definitions, port bindings, and the main simulation driver.

2. **Module hierarchy**:
   - `RawTLP` struct: The basic data packet structure with custom operators
   - `iRC`: Root Complex module (sender) with credit monitoring and packet generation
   - `iEP`: Endpoint module (receiver) with FIFO management and credit generation

3. **Understanding module implementation**:
   - Each module has input/output port declarations
   - Internal state variables including credit counters and FIFO queues
   - SC_THREAD processes that implement functionality
   - Constructor with thread registration and sensitivity lists

4. **Focus on control flow**:
   - Trace the credit flow from EP to RC via the 3-bit credit bus
   - Trace the data flow from RC to EP via raw_valid and raw_tlp signals
   - Understand how round-robin arbitration works in the RC sender thread

#### Git Version History Commands

To understand the project's evolution through git:

```bash
# View commit history with descriptions
git log --oneline

# View detailed changes in a specific commit
git show <commit-hash>

# See how a specific file evolved
git log --follow -p src/main.cpp

# Compare two versions
git diff <commit1> <commit2>

# View branches and their relationships
git log --graph --oneline --all
```

#### Project Evolution Patterns

This project typically evolves in the following patterns:

1. **Multi-clock → Single clock**: Earlier versions may have used separate clock domains, current version uses unified clocking
2. **Basic credit → Smart credit**: Evolution from simple credit counting to sophisticated FIFO space management
3. **Print debugging → Clean simulation**: Removal of debug prints for cleaner waveform analysis
4. **Manual timing → WaveDrom**: Addition of standardized timing diagram formats

When analyzing changes between versions, focus on:
- Changes to the credit management algorithms
- Evolution of the arbitration mechanism in RC
- Modifications to FIFO management in EP
- Addition of timing analysis and visualization tools 