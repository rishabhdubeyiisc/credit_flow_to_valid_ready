# Credit-Based Flow-Control & Valid/Ready Demo

SystemC simulation illustrating two independent data-transport topologies built from the same building blocks:

1. *Pure credit-based link*
   iRC (root complex) ➜ iEP (endpoint)

2. *Hybrid link using ready/valid in the middle*
   iRC_tx (root complex) ➜ TX (credit FIFO front-end + ready/valid transmitter) ➜ RX (wide FIFO consumer) ➜ iEP_after_RX (per-thread consumer)

The two paths share the same clock and run side-by-side so you can directly compare latency, throughput, back-pressure behaviour and waveform size.

```
                credits[2:0]
        ┌───────────────────────────────┐
        │                               │
 iRC ──▶│ raw_valid/tlp     iEP         │
        └───────────────────────────────┘


 iRC_tx ─raw_valid/tlp─▶ TX ─valid/ready─▶ RX ─raw_valid/tlp─▶ iEP_after_RX
                 ▲          ▲                    ▲                 ▲
                 └─credit───┘                    └─credit[2:0]─────┘
```

## Module cheat-sheet
* **iRC / iRC_tx** – round-robin sender, maintains one credit counter per thread and prints `Send seq=n tid=t` on every packet.
* **Threaded_Queue** – core building block used by iEP, TX and RX. Holds one FIFO (depth 8) per logical thread and raises a one-clock credit pulse when space becomes available.
* **iEP / iEP_after_RX** – router ➜ three `Threaded_Queue`s ➜ popper that removes **one packet per queue every 4th clock**; credits are OR-reduced back to the upstream module.
* **TX** – mirrors the iEP front-end but instead of the slow popper has a transmitter that forwards **one packet per cycle** when `egress_ready` is asserted; credits return to iRC_tx.
* **RX** – 1024-deep FIFO per thread, drives `ready_out` when any FIFO has space and converts ready/valid back to the credit protocol for iEP_after_RX.

## Key parameters (edit in `src/main.cpp`)
* `Threaded_Queue::FIFO_CAPACITY` – per-thread depth (default 8)
* `RX::RX_FIFO_CAPACITY` – depth of RX FIFOs (default 1024)
* Pop cadence in iEP/iEP_after_RX – controlled by `if (pop_counter == 3)` (¼ rate)
* `g_enable_popping` – global toggle to disable the popper at runtime.

## Build & Run
```bash
# Edit SYSTEMC_INC / SYSTEMC_LIB in Makefile if your SystemC is not in /usr/local
make          # builds build/sim
./build/sim   # runs ~20 µs and emits two VCD files
```

### Waveforms
```bash
gtkwave irc_iep_flow.vcd      # iRC ➜ iEP (pure-credit path)
gtkwave irc_tx_flow.vcd       # iRC_tx ➜ TX ➜ RX ➜ iEP_after_RX (hybrid path)
```

## Sanity checklist
1. Sequence numbers rise monotonically and match between sender and receiver.
2. Credit buses on both paths toggle continuously – never stuck.
3. No packet carries `thread_id == 0` or `seq_num == 0`.
4. The iEP popper prints every 4th clock and each `Threaded_Queue` credit counter returns to *8* after every pop group.

## Extending the demo
* Increase the number of logical threads by widening `RawTLP.thread_id` and updating loop bounds.
* Stress congestion: set `g_enable_popping = false` after a few µs to watch queues fill up.
* Replace the simple `TX` block with any RTL you want to characterise – the credit front-end is unaffected.

---

This README doubles as a compact primer when you prompt an LLM with the project – it should remain self-contained, architecture-focused and free of build-system minutiae.
