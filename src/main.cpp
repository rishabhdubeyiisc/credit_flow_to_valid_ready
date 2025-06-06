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
    sc_signal<sc_uint<3>> iEPcredit_bus;
    sc_signal<bool>       Credit_packer2CNOC_valid, CNOC2Credit_packer_ready;
    sc_signal<AxiWord>    Credit_packer2CNOC_AXI_data;
    sc_signal<bool>       CNOC2Credit_Pulser_valid, CreditPulser2CNOC_ready;
    sc_signal<AxiWord>    CNOC2Credit_Pulser_axi_data;
    sc_signal<sc_uint<3>> iRCcredit_bus;

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
    ep_rx.credit_out(iEPcredit_bus);

    // Credit path over its own deterministic AXI NoC
    CreditTx Credit_packer("Credit_packer", CREDIT_SENSE_WINDOW);
    Credit_packer.clk(system_clk);
    Credit_packer.reset_n(reset_n);
    Credit_packer.credit_in(iEPcredit_bus);
    Credit_packer.valid_out(Credit_packer2CNOC_valid);
    Credit_packer.axi_out(Credit_packer2CNOC_AXI_data);
    Credit_packer.ready_in(CNOC2Credit_packer_ready);

    // Use a separate NoC instance but same behaviour
    AxiNoC c_noc("CNOC", CREDIT_NOC_LATENCY, NOC_PATTERN_LEN, CREDIT_NOC_STALL_PCT);
    c_noc.clk(system_clk);
    c_noc.reset_n(reset_n);
    c_noc.valid_in(Credit_packer2CNOC_valid);
    c_noc.axi_in(Credit_packer2CNOC_AXI_data);
    c_noc.ready_out(CNOC2Credit_packer_ready);
    c_noc.valid_out(CNOC2Credit_Pulser_valid);
    c_noc.axi_out(CNOC2Credit_Pulser_axi_data);
    c_noc.ready_in(CreditPulser2CNOC_ready);

    CreditRx Credit_Pulser("Credit_Pulser");
    Credit_Pulser.clk(system_clk);
    Credit_Pulser.reset_n(reset_n);
    Credit_Pulser.valid_in(CNOC2Credit_Pulser_valid);
    Credit_Pulser.axi_in(CNOC2Credit_Pulser_axi_data);
    Credit_Pulser.ready_out(CreditPulser2CNOC_ready);
    Credit_Pulser.credit_out(iRCcredit_bus);

    // Connect to iRC_tx
    rc_tx.credit_in(iRCcredit_bus);

    // ------------------- Data path NoC (TX → RX) ---------------------------
    AxiNoC noc("AXI_NOC", DATA_NOC_LATENCY, NOC_PATTERN_LEN, DATA_NOC_STALL_PCT);
    noc.clk(system_clk);
    noc.reset_n(reset_n);
    noc.valid_in(TX2NOC_valid);
    noc.axi_in(TX2NOC_axi);
    noc.ready_out(NOC2TX_ready);
    noc.valid_out(NOC2RX_valid);
    noc.axi_out(NOC2RX_axi);
    noc.ready_in(RX2NOC_ready);

    // Create trace files
    sc_trace_file* tf = sc_create_vcd_trace_file("irc_iep");
    tf->set_time_unit(1, SC_NS);
    sc_trace_file* tf_tx = sc_create_vcd_trace_file("noc_flow");
    tf_tx->set_time_unit(1, SC_NS);

    // Trace iRC->iEP signals
    sc_trace(tf, system_clk,         "system_clk");  // Trace the common clock
    sc_trace(tf, reset_n,            "reset_n");
    sc_trace(tf, raw_valid,          "raw_valid");
    sc_trace(tf, raw_tlp,            "raw_tlp");  // This will now use our custom tracing function
    sc_trace(tf, credit,             "credit");

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
    // Credit path traces
    sc_trace(tf_tx, Credit_packer2CNOC_valid, "Credit_packer2CNOC_valid");
    sc_trace(tf_tx, CNOC2Credit_packer_ready, "CNOC2Credit_packer_ready");
    sc_trace(tf_tx, Credit_packer2CNOC_AXI_data,   "Credit_packer2CNOC_AXI_data");
    sc_trace(tf_tx, CNOC2Credit_Pulser_valid, "CNOC2Credit_Pulser_valid");
    sc_trace(tf_tx, CreditPulser2CNOC_ready, "CreditPulser2CNOC_ready");
    sc_trace(tf_tx, CNOC2Credit_Pulser_axi_data,   "CNOC2Credit_Pulser_axi_data");
    sc_trace(tf_tx, iRCcredit_bus, "iRCcredit_bus");
    sc_trace(tf_tx, iEPcredit_bus, "iEPcredit_bus");
    sc_trace(tf_tx, c_noc.stall_active_sig, "CNOC_stall_active");
    sc_trace(tf_tx, c_noc.delta_cycle_ctr, "CNOC_delta_cycle_ctr");
    sc_trace(tf_tx, noc.stall_active_sig, "DATA_NOC_stall_active");
    sc_trace(tf_tx, noc.delta_cycle_ctr, "DATA_NOC_delta_cycle_ctr");

    // Duty cycle monitor instance
    CreditDutyMon mon("CreditMon");
    mon.clk(system_clk);
    mon.bus_direct(credit);
    mon.bus_hybrid(iRCcredit_bus);

    // Enable per-module tracing
    std::cout << "Setting up per-module tracing..." << std::endl;
    rc.setup_tracing(true);                    // iRC_trace.vcd
    ep.setup_tracing(true);                    // iEP_trace.vcd
    rc_tx.setup_tracing(true);                 // iRC_tx_trace.vcd
    ep_rx.setup_tracing(true);                 // iEP_after_RX_trace.vcd
    tx_fifo.setup_tracing(true);               // TX_trace.vcd
    rx_fifo.setup_tracing(true);               // RX_trace.vcd
    Credit_packer.setup_tracing(true);         // Credit_packer_trace.vcd
    Credit_Pulser.setup_tracing(true);         // Credit_Pulser_trace.vcd
    c_noc.setup_tracing(true);                 // CNOC_trace.vcd
    noc.setup_tracing(true);                   // AXI_NOC_trace.vcd
    mon.setup_tracing(true);                   // CreditMon_trace.vcd

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

