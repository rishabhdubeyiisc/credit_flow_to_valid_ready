# Credit-Based vs Ready/Valid – SystemC Playground

This repo is a **minimal yet complete** experiment-bed that lets you compare two flow-control philosophies side-by-side under the *same* traffic generator and consumer logic.

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

Both topologies share one clock and run for 20 µs; you can watch every wave in **GTKW** and mine every statistic with the included Python tools.

---
## 1.  Modules (all in `src/main.cpp`)

| Module            | Purpose / Notes |
|-------------------|-----------------|
| `iRC` / `iRC_tx`  | Round-robin multi-thread sender. One credit counter per thread; prints `Send seq=n tid=t`. |
| `Threaded_Queue`  | 8-deep FIFO + credit logic for **one** thread. Building block for iEP & front-ends. |
| `ThreadedFrontEnd`| Router + **3×** `Threaded_Queue` + credit OR-combine. Re-used by both iEP flavours. |
| `SimpleTxFIFO`    | *Single* FIFO that converts Raw-valid/TLP into ready/valid. Depth tunable (default 24). |
| `SimpleRxFIFO`    | Reverse converter (ready/valid → Raw-valid/TLP). Depth tunable (default 2). |
| `ProxyCreditGen`  | "Leaky-bucket" proxy that **continues counting** credits while it emits buffered ones; 8-cycle stats window. Breaks the long credit loop safely. |
| `AxiNoC`          | 12-stage pipeline that asserts `ready` low for `NOC_STALL_CYC` cycles and high for `NOC_READY_CYC` cycles, modelling round-robin arbitration. |
| `iEP`             | Classical credit consumer. Pops one pkt/queue every 4 clks. |
| `iEP_after_RX`    | Same as iEP but fed through TX/RX path. |

---
## 2.  Key knobs

| Symbol                                | Description | Default |
|---------------------------------------|-------------|---------|
| `GLOBAL_SENSE_WINDOW`                 | Statistics window in `ProxyCreditGen` (credits keep flowing even while sensing). | 8 |
| `TX_FIFO_DEPTH`                       | Burst buffer at TX; ***should cover stall latency*** (48 fits 12-cycle NoC × 4 beats). | 48 |
| `RX_FIFO_DEPTH`                       | Elasticity buffer after network. | 24 |
| `NOC_STALL_CYC`/`NOC_READY_CYC`       | Deterministic ready-low / ready-high period in `AxiNoC`. | 6 / 6 |
| `NOC_STALL_PCT`                       | Duty-cycle of ready-low in `AxiNoC` (0-99 %). | 10 % |
| `Threaded_Queue::FIFO_CAPACITY`       | Per-thread depth in iEPs. | 8 |
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

1. **Bandwidth-delay product** – raise `SimpleTxFIFO::DEPTH` while inserting artificial latency blocks between TX and RX.
2. **Congestion** – disable popping in `iEP_after_RX` to see credit starvation propagate back through the network.
3. **Thread scaling** – widen `RawTLP.thread_id`, update loop bounds, and observe how credit bus width vs window sizing changes buffer requirements.
4. **Algorithmic tuning** – change the emit rule in `ProxyCreditGen` (e.g. emit 2 credits/queue/clk) and compare results with the tuner.

---
### This README as an AI prompt
The document now contains:
* full architectural diagram
* module cheat-sheet
* all tunable parameters & defaults
* how to build, run, analyse and sweep

so pasting it into an LLM gives enough context to propose new modules or debug future changes without reading the entire codebase.
