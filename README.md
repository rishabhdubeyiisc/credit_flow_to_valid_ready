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
| `iEP`             | Classical credit consumer. Pops one pkt/queue every 4 clks. |
| `iEP_after_RX`    | Same as iEP but fed through TX/RX path. |

---
## 2.  Key knobs

| Symbol                                | Description | Default |
|---------------------------------------|-------------|---------|
| `GLOBAL_SENSE_WINDOW`                 | Statistics window in `ProxyCreditGen` (credits keep flowing even while sensing). | 8 |
| `SimpleTxFIFO::DEPTH`                 | Burst buffer at TX; ***must be ≥ threads × window*** (24 for 3×8). | 24 |
| `SimpleRxFIFO::DEPTH`                 | Elasticity buffer after network; 1–2 is enough. | 2 |
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

`scripts/fifo_tuner.py` rewrites the TX & RX DEPTH constants, rebuilds, runs, parses and dumps a CSV (`fifo_sweep_report.csv`).

```bash
python3 scripts/fifo_tuner.py

TX_DEPTH RX_DEPTH  Ready(Mpps)  Ratio_to_Credit
---------------------------------------------
       2        1   ---           N/A   PARSE_ERR
       2        2       5.20       0.97 OK
       4        1   ---           N/A   PARSE_ERR
       ...
```
*OK* marks configurations where ready-path ≥ 90 % of credit-path throughput.

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
