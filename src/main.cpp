#include <systemc.h>
#include <iomanip>
#include <queue>  // For std::queue
#include <iostream>  // For std::cout
#include <string>
#include <algorithm>
#include <cstdlib>
#include "payloads.h"
#include "modules.h"
#include "config.h"

bool GlobalConfig::enable_popping = true;

// Top-level sc_main
int sc_main(int argc, char* argv[]) {

    sc_clock           system_clk("system_clk", 100, SC_NS);  // Single common clock
    sc_signal<bool>    reset_n;
    sc_signal<sc_uint<3>> credit;  // 3-bit credit bus
    sc_signal<bool>    raw_valid;
    sc_signal<RawTLP>  raw_tlp;

    // Create trace files
    sc_trace_file* tf = sc_create_vcd_trace_file("irc_iep_flow");
    tf->set_time_unit(1, SC_NS);
    sc_trace_file* tf_tx = sc_create_vcd_trace_file("irc_tx_flow");
    tf_tx->set_time_unit(1, SC_NS);

    // Create and connect iRC->iEP topology
    iRC rc("iRC");
    rc.clk(system_clk);  // Connect RC to common clock
    rc.reset_n(reset_n);
    rc.credit_in(credit);
    rc.raw_valid(raw_valid);
    rc.raw_tlp(raw_tlp);

    iEP ep("iEP", THREAD_Q_DEPTH);
    ep.clk(system_clk);  // Connect EP to common clock
    ep.reset_n(reset_n);
    ep.raw_valid(raw_valid);
    ep.raw_tlp(raw_tlp);
    ep.credit_out(credit);

    // Trace iRC->iEP signals
    sc_trace(tf, system_clk,         "system_clk");  // Trace the common clock
    sc_trace(tf, reset_n,            "reset_n");
    sc_trace(tf, raw_valid,          "raw_valid");
    sc_trace(tf, raw_tlp,            "raw_tlp");  // This will now use our custom tracing function
    sc_trace(tf, credit,             "credit");

    // Signals for TX/RX simple path
    sc_signal<bool>    RC2TX_raw_valid;
    sc_signal<RawTLP>  RC2TX_raw_tlp;
    sc_signal<bool>    TX2NOC_valid;
    sc_signal<AxiWord> TX2NOC_axi;
    sc_signal<bool>    NOC2TX_ready;
    sc_signal<bool>    NOC2RX_valid;
    sc_signal<AxiWord> NOC2RX_axi;
    sc_signal<bool>    RX2NOC_ready;
    sc_signal<bool>    RX2EP_valid;
    sc_signal<RawTLP>  RX2EP_tlp;
    sc_signal<sc_uint<3>> credit_iEP2cTx;
    sc_signal<bool>       c_valid_tx, c_ready_tx;
    sc_signal<AxiWord>    c_axi_tx;
    sc_signal<bool>       c_valid_rx, c_ready_rx;
    sc_signal<AxiWord>    c_axi_rx;
    sc_signal<sc_uint<3>> credit_pkt2rc;

    // Create and connect TX path with single FIFO
    iRC rc_tx("iRC_tx");
    rc_tx.clk(system_clk);
    rc_tx.reset_n(reset_n);
    rc_tx.raw_valid(RC2TX_raw_valid);
    rc_tx.raw_tlp(RC2TX_raw_tlp);

    SimpleTxFIFO tx_fifo("TX", TX_FIFO_DEPTH);
    tx_fifo.clk(system_clk);
    tx_fifo.reset_n(reset_n);
    tx_fifo.ingress_valid(RC2TX_raw_valid);
    tx_fifo.ingress_tlp(RC2TX_raw_tlp);
    tx_fifo.egress_valid(TX2NOC_valid);
    tx_fifo.egress_axi(TX2NOC_axi);
    tx_fifo.egress_ready(NOC2TX_ready);

    // RX simple FIFO
    SimpleRxFIFO rx_fifo("RX", RX_FIFO_DEPTH);
    rx_fifo.clk(system_clk);
    rx_fifo.reset_n(reset_n);
    rx_fifo.valid_in(NOC2RX_valid);
    rx_fifo.axi_in(NOC2RX_axi);
    rx_fifo.ready_out(RX2NOC_ready);
    rx_fifo.valid_out(RX2EP_valid);
    rx_fifo.tlp_out(RX2EP_tlp);

    // iEP instance after RX (consumes TX path)
    iEP ep_rx("iEP_after_RX", THREAD_Q_DEPTH);
    ep_rx.clk(system_clk);
    ep_rx.reset_n(reset_n);
    ep_rx.raw_valid(RX2EP_valid);
    ep_rx.raw_tlp(RX2EP_tlp);
    ep_rx.credit_out(credit_iEP2cTx);

    // Credit path over its own deterministic AXI NoC
    CreditTx c_tx("CreditTx", GLOBAL_SENSE_WINDOW);
    c_tx.clk(system_clk);
    c_tx.reset_n(reset_n);
    c_tx.credit_in(credit_iEP2cTx);
    c_tx.valid_out(c_valid_tx);
    c_tx.axi_out(c_axi_tx);
    c_tx.ready_in(c_ready_tx);

    // Use a separate NoC instance but same behaviour
    AxiNoC c_noc("Credit_NOC", NOC_STATIC_LATENCY_ONE_WAY, NOC_PATTERN_LEN, NOC_STALL_PCT);
    c_noc.clk(system_clk);
    c_noc.reset_n(reset_n);
    c_noc.valid_in(c_valid_tx);
    c_noc.axi_in(c_axi_tx);
    c_noc.ready_out(c_ready_tx);
    c_noc.valid_out(c_valid_rx);
    c_noc.axi_out(c_axi_rx);
    c_noc.ready_in(c_ready_rx);

    CreditRx c_rx("CreditRx");
    c_rx.clk(system_clk);
    c_rx.reset_n(reset_n);
    c_rx.valid_in(c_valid_rx);
    c_rx.axi_in(c_axi_rx);
    c_rx.ready_out(c_ready_rx);
    c_rx.credit_out(credit_pkt2rc);

    // Connect to iRC_tx
    rc_tx.credit_in(credit_pkt2rc);

    // ------------------- Data path NoC (TX → RX) ---------------------------
    AxiNoC noc("AXI_NOC", NOC_STATIC_LATENCY_ONE_WAY, NOC_PATTERN_LEN, NOC_STALL_PCT);
    noc.clk(system_clk);
    noc.reset_n(reset_n);
    noc.valid_in(TX2NOC_valid);
    noc.axi_in(TX2NOC_axi);
    noc.ready_out(NOC2TX_ready);
    noc.valid_out(NOC2RX_valid);
    noc.axi_out(NOC2RX_axi);
    noc.ready_in(RX2NOC_ready);

    // traces – entire TX→NoC→RX path plus proxy credits
    sc_trace(tf_tx, system_clk,         "system_clk");
    sc_trace(tf_tx, reset_n,            "reset_n");
    sc_trace(tf_tx, RC2TX_raw_valid, "RC2TX_raw_valid");
    sc_trace(tf_tx, RC2TX_raw_tlp,  "RC2TX_raw_tlp");
    sc_trace(tf_tx, TX2NOC_valid,    "TX2NOC_valid");
    sc_trace(tf_tx, NOC2TX_ready, "NOC2TX_ready");
    sc_trace(tf_tx, TX2NOC_axi,   "TX2NOC_axi");
    sc_trace(tf_tx, NOC2RX_valid,  "NOC2RX_valid");
    sc_trace(tf_tx, NOC2RX_axi,    "NOC2RX_axi");
    sc_trace(tf_tx, RX2NOC_ready,  "RX2NOC_ready");
    sc_trace(tf_tx, RX2EP_valid, "RX2EP_valid");
    sc_trace(tf_tx, RX2EP_tlp,   "RX2EP_tlp");
    // sc_trace(tf_tx, c_valid_tx, "credit_valid_tx");
    // sc_trace(tf_tx, c_ready_tx, "credit_ready_tx");
    // sc_trace(tf_tx, c_valid_rx, "credit_valid_rx");
    // sc_trace(tf_tx, c_ready_rx, "credit_ready_rx");
    // sc_trace(tf_tx, c_axi_tx,   "credit_axi_tx");
    // sc_trace(tf_tx, c_axi_rx,   "credit_axi_rx");
    // sc_trace(tf_tx, credit_pkt2rc, "credit_pkt2rc");
    // sc_trace(tf_tx, credit_iEP2cTx, "credit_iEP2cTx");
    // sc_trace(tf_tx, noc.stall_active_sig, "AXI_NOC_stall_active");
    // sc_trace(tf_tx, noc.delta_cycle_ctr, "AXI_NOC_delta_cycle_ctr");

    // Duty cycle monitor instance
    CreditDutyMon mon("CreditMon");
    mon.clk(system_clk);
    mon.bus_direct(credit);
    mon.bus_hybrid(credit_pkt2rc);

    // Initial values
    reset_n.write(false);
    
    // Apply reset for 20ns
    sc_start(20, SC_NS);
    reset_n.write(true);
    
    // Phase-1 : run for first half of sim_time_in_us with normal operation
    const unsigned phase1_us = sim_time_in_us / 2;
    sc_start(static_cast<double>(phase1_us), SC_US);

    // Disable queue popping in iEP so that no new credits are generated
    GlobalConfig::enable_popping = false;
    std::cout << "*** Disabled iEP popping at " << sc_time_stamp() << " ***" << std::endl;

    // Phase-2 : let pipeline drain for the remaining time
    sc_start(static_cast<double>(sim_time_in_us - phase1_us), SC_US);

    // Print duty cycle stats
    mon.report();

    // Close trace files
    sc_close_vcd_trace_file(tf);
    sc_close_vcd_trace_file(tf_tx);

    return 0;
}

