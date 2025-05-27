# Credit-Based Flow-Control Demo (iRC / TX / iEP / RX)

This SystemC simulation now contains **two independent data paths** so you can compare classic credit-based flow control with a conventional valid/ready handshake.

```
 (iRC) ─raw_valid / raw_tlp──▶  (iEP)
                    ▲                ▲
                    └─ credit[2:0] ──┘

 (iRC_tx) ─raw_valid / raw_tlp─▶ (TX) ─valid / ready─▶ (RX)
                       ▲                    ▲
                       └─ credit[2:0] ─────┘
```

* `iRC` and `iRC_tx` are identical three-thread senders; they differ only in the sink they feed.
* `iEP` and `TX` each contain **three** thread FIFOs (`Threaded_Queue`, depth = 8).
* `RX` has one large FIFO (depth = 1024) and provides back-pressure via `ready_out`.

---
## Packet format
```cpp
struct RawTLP {
    sc_uint<32> seq_num;   // unique sequence number
    sc_uint<2>  thread_id; // 1-3 selects logical thread
};
```

---
## Threaded_Queue (per-thread FIFO + credit source)
Single `SC_THREAD main_thread()` per FIFO:
1. Wait pos-edge → wait 1 Δ-cycle → sample `valid_in`, enqueue if space.
2. After a pop, assert `credit_out` high for the following **full clock**.
3. Maintain a simple `credits` counter (no separate inc/dec flags).

Parameters
```
FIFO_CAPACITY = 8       // depth of each per-thread FIFO
```

---
## Module summary
* **iRC / iRC_tx** – keeps one credit counter per thread; round-robin sender prints `Send seq=n tid=t` once per packet.
* **iEP** – router ➜ per-thread FIFO ➜ popper.  Popper removes one packet per queue every 4th clock; credits are OR-combined back to `iRC`.
* **TX** – same FIFO front-end; transmitter pops **one packet/clock** when `RX.ready_out` is high and forwards it to RX; credits return to `iRC_tx`.
* **RX** – 1024-deep FIFO; drives `ready_out` when space exists; prints every packet with FIFO occupancy.

---
## Building & running
```bash
# edit SYSTEMC_INC / SYSTEMC_LIB in Makefile if needed
make            # builds src/main.cpp → build/sim
./build/sim     # runs; console always prints packet flow
```
Waveforms
```bash
gtkwave irc_iep_flow.vcd   # iRC → iEP path
gtkwave irc_tx_flow.vcd    # iRC_tx → TX → RX path
```

---
## Typical console excerpt
```
300 ns [iRC] Send seq=1 tid=1
300 ns [iEP] Route: seq_num=1 tid=1 -> queue 0
400 ns [iEP popper] counter=3
400 ns [iEP.iEP_queue_0] Popping data: seq_num=1 …

300 ns [iRC_tx] Send seq=1 tid=2
300 ns [TX] Route: seq_num=1 tid=2 -> queue 1
500 ns [TX] Hold pkt seq=1 tid=2 from queue 1
500 ns [TX] RX accepted seq=1
500 ns RX: Received seq_num=1 tid=2 (occ=1/1024)
```

---
## Tuning knobs
* **Pop rate**  (iEP) – change the popper condition `if (pop_counter == 3)` and modulus `4`.
* **FIFO depths** – `Threaded_Queue::FIFO_CAPACITY`, `RX_FIFO_CAPACITY`.
* **Log volume** – all prints use `std::cout`; comment out lines you do not need.

---
## Sanity checklist
1. `raw_tlp_iRC2TX.seq_num` and `TX2RX_tlp.seq_num` rise indefinitely and match (offset by pipeline latency).
2. Both credit buses toggle continuously – never stuck.
3. No packet with `thread_id = 0` or `seq_num = 0` appears.
4. Popper prints every fourth clock and each FIFO's credit count returns to 8 after every pop group.

If any of these conditions fail revisit the per-queue valid-pulse logic in **iEP** and **TX**. 