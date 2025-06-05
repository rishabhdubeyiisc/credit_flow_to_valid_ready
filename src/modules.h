#ifndef MODULES_H
#define MODULES_H

#include <systemc.h>
#include "payloads.h"
#include "config.h"

SC_MODULE(Threaded_Queue) {
    sc_in<bool> clk;
    sc_in<bool> reset_n;
    sc_in<RawTLP> raw_tlp_in;
    sc_in<bool> valid_in;
    sc_out<bool> credit_out;

    sc_fifo<RawTLP> fifo;
    const unsigned int capacity;
    
    // Credit state
    unsigned int credits;
    bool credit_pending;

    void main_thread();

    // Method to check if data is available
    bool has_data() const ;

    // Method to pop data from FIFO
    bool pop_data(RawTLP& pkt) ;

    SC_CTOR(Threaded_Queue, unsigned int capacity) : fifo(capacity), capacity(capacity) {
        SC_THREAD(main_thread);
        sensitive << clk.pos();
    }
};


// -----------------------------------------------------------------------------
// ThreadedFrontEnd: reusable wrapper that contains 3 Threaded_Queues plus the
// router (valid/tlp -> per-queue) and credit combiner logic.  This captures the
// functionality that was previously duplicated inside both iEP and TX.
// -----------------------------------------------------------------------------

SC_MODULE(ThreadedFrontEnd) {
    // Ports identical to the classic front-end used by iEP / TX
    sc_in<bool>         clk;
    sc_in<bool>         reset_n;
    sc_in<bool>         ingress_valid;
    sc_in<RawTLP>       ingress_tlp;
    sc_out<sc_uint<3>>  credit_out;

    // Internal per-thread FIFOs
    Threaded_Queue* queues[3];
    sc_signal<bool>      credit_signals[3];
    sc_signal<RawTLP>    tlp_signals[3];
    sc_signal<bool>      valid_signals[3];

    // Exposed helpers so outer modules can pull data deterministically
    bool has_data(int idx) const ;

    bool pop_data(int idx, RawTLP& pkt);

    // Router thread: demux ingress packet by thread_id -> per-queue signals
    void input_router_thread();

    // Combiner: OR-reduce per-queue credit pulses into a 3-bit bus
    void credit_combine_thread() ;

    SC_CTOR(ThreadedFrontEnd, unsigned queue_capacity) {
        // Build child queue names without illegal '.' characters to avoid SystemC W506.
        std::string prefix(name());
        std::replace(prefix.begin(), prefix.end(), '.', '_');
        for (int i = 0; i < 3; ++i) {
            std::string qn = prefix + "_queue_" + std::to_string(i);
            queues[i] = new Threaded_Queue(qn.c_str(), queue_capacity);
            queues[i]->clk(clk);
            queues[i]->reset_n(reset_n);
            queues[i]->raw_tlp_in(tlp_signals[i]);
            queues[i]->valid_in(valid_signals[i]);
            queues[i]->credit_out(credit_signals[i]);
        }

        SC_THREAD(input_router_thread);
        sensitive << clk.pos();

        SC_THREAD(credit_combine_thread);
        sensitive << clk.pos();
    }
};

// -----------------------------------------------------------------------------
// SimpleTxFIFO: single FIFO buffer with valid/ready handshake on egress.
// -----------------------------------------------------------------------------

SC_MODULE(SimpleTxFIFO) {
    sc_in<bool>        clk;
    sc_in<bool>        reset_n;
    // ingress from iRC_tx (raw_valid/raw_tlp)
    sc_in<bool>        ingress_valid;
    sc_in<RawTLP>      ingress_tlp;
    // egress toward RX (valid/ready)
    sc_out<bool>       egress_valid;
    sc_out<AxiWord>    egress_axi;
    sc_in<bool>        egress_ready;

    sc_fifo<RawTLP> fifo;
    unsigned int max_occ = 0;

    void main_thread();

    SC_CTOR(SimpleTxFIFO, unsigned depth) : fifo(depth) {
        SC_THREAD(main_thread);
        sensitive << clk.pos();
    }
};


// -----------------------------------------------------------------------------
// SimpleRxFIFO: single FIFO buffer; accepts valid/ready, outputs raw_valid/raw_tlp.
// -----------------------------------------------------------------------------

SC_MODULE(SimpleRxFIFO) {
    sc_in<bool>        clk;
    sc_in<bool>        reset_n;
    // ingress from TX
    sc_in<bool>        valid_in;
    sc_in<AxiWord>     axi_in;
    sc_out<bool>       ready_out;
    // egress toward iEP_after_RX
    sc_out<bool>       valid_out;
    sc_out<RawTLP>     tlp_out;

    sc_fifo<RawTLP> fifo;
    unsigned int max_occ = 0;

    void main_thread() ;

    SC_CTOR(SimpleRxFIFO, unsigned depth) : fifo(depth) {
        SC_THREAD(main_thread);
        sensitive << clk.pos();
    }
};

// -----------------------------------------------------------------------------
// CreditTx: senses credit pulses near iEP and emits them as one AXI beat.
// -----------------------------------------------------------------------------

SC_MODULE(CreditTx){
    sc_in<bool>        clk;
    sc_in<bool>        reset_n;
    sc_in<sc_uint<3>>  credit_in;   // pulses from iEP queues
    // AXI Stream out
    sc_out<bool>       valid_out;
    sc_out<AxiWord>    axi_out;
    sc_in<bool>        ready_in;

    sc_uint<16> accum[3] = {0,0,0};
    unsigned ctr = 0;
    bool sending = false;
    AxiWord pending;
    const unsigned window_size;

    void main_thread();
    
    SC_CTOR(CreditTx, unsigned window) : window_size(window) {
        SC_THREAD(main_thread);
        sensitive<<clk.pos();
    }
};

// -----------------------------------------------------------------------------
// CreditRx: converts AXI credit packet back into per-thread pulses for iRC_tx
// -----------------------------------------------------------------------------

SC_MODULE(CreditRx){
    sc_in<bool>        clk;
    sc_in<bool>        reset_n;
    // AXI Stream in
    sc_in<bool>        valid_in;
    sc_in<AxiWord>     axi_in;
    sc_out<bool>       ready_out;
    // Credit pulses toward RC
    sc_out<sc_uint<3>> credit_out;

    sc_uint<16> emit_cnt[3] = {0,0,0};

    void main_thread();

    SC_CTOR(CreditRx){
        SC_THREAD(main_thread);
        sensitive<<clk.pos();
    }
};

// -----------------------------------------------------------------------------
// iRC: Root Complex module (sender)
// -----------------------------------------------------------------------------

SC_MODULE(iRC) {
    sc_in<bool>         clk;
    sc_in<bool>         reset_n;
    
    sc_in<sc_uint<3>>   credit_in;  // 3-bit credit bus for 3 threads
    sc_out<bool>        raw_valid;
    sc_out<RawTLP>      raw_tlp;

    // Internal state
    int packet_seq;
    int credit_counter[3];  // Array of credit counters for each thread
    sc_event credit_event;  // Event to signal when any credit is received

    // Thread for monitoring credit pulses
    void credit_monitor_thread() ;

    // Thread for sending TLPs
    void sender_thread() ;
    
    SC_CTOR(iRC) {
        // Register the threads
        SC_THREAD(credit_monitor_thread);
        sensitive << clk.pos() << credit_in;;  // Only sensitive to clock edges
        
        SC_THREAD(sender_thread);
        sensitive << clk.pos() << credit_event;
    }
};

// iEP: Endpoint module (receiver)
SC_MODULE(iEP) {
    sc_in<bool>         clk;
    sc_in<bool>         reset_n;
    sc_in<bool>         raw_valid;
    sc_in<RawTLP>       raw_tlp;
    sc_out<sc_uint<3>>  credit_out;

    ThreadedFrontEnd* threaded_queues;

    // Method to process popped data
    void process_popped_data(const RawTLP& pkt, int queue_id) ;

    void popper_thread() ;

    SC_CTOR(iEP, unsigned queue_capacity) {
        threaded_queues = new ThreadedFrontEnd((std::string(name()) + "_front").c_str(), queue_capacity);
        threaded_queues->clk(clk);
        threaded_queues->reset_n(reset_n);
        threaded_queues->ingress_valid(raw_valid);
        threaded_queues->ingress_tlp(raw_tlp);
        threaded_queues->credit_out(credit_out);

        SC_THREAD(popper_thread);
        sensitive << clk.pos() << reset_n;
    }
};

// -----------------------------------------------------------------------------
// AxiNoC: simple elastic buffer that injects random back-pressure (ready=0)
// cycles to emulate a network. Parameter MAX_STALL controls worst-case stall
// between beats.
// -----------------------------------------------------------------------------

SC_MODULE(AxiNoC) {
    sc_in<bool>        clk;
    sc_in<bool>        reset_n;
    // upstream (TX) side
    sc_in<bool>        valid_in;
    sc_in<AxiWord>     axi_in;
    sc_out<bool>       ready_out;
    // downstream (RX) side
    sc_out<bool>       valid_out;
    sc_out<AxiWord>    axi_out;
    sc_in<bool>        ready_in;

    // Internal state
    const unsigned PIPE_LAT; // latency cycles
    AxiWord* pipe;
    bool* pipe_valid;
    unsigned delta_cycle_ctr = 0;
    bool stall_active_sig = false;
    unsigned pattern_ctr = 0;
    bool is_main_noc;
    const unsigned NOC_PATTERN_LEN;
    const unsigned NOC_STALL_PCT;

    void main_thread();

    SC_CTOR(AxiNoC, unsigned latency, unsigned pattern_len = 100, unsigned stall_pct = 15) 
        : PIPE_LAT(latency), is_main_noc(std::string(name()) == "AXI_NOC"),
          NOC_PATTERN_LEN(pattern_len), NOC_STALL_PCT(stall_pct) {
        pipe = new AxiWord[PIPE_LAT];
        pipe_valid = new bool[PIPE_LAT];
        for(unsigned i = 0; i < PIPE_LAT; i++) {
            pipe_valid[i] = false;
        }
        SC_THREAD(main_thread);
        sensitive << clk.pos();
    }

    ~AxiNoC() {
        delete[] pipe;
        delete[] pipe_valid;
    }
};


// -----------------------------------------------------------------------------
// CreditDutyMon: measures duty cycle (percentage of cycles bus != 0)
// -----------------------------------------------------------------------------

SC_MODULE(CreditDutyMon){
    sc_in<bool>        clk;
    sc_in<sc_uint<3>>  bus_direct;
    sc_in<sc_uint<3>>  bus_hybrid;

    sc_uint<64> total      = 0;
    sc_uint<64> hi_direct  = 0;
    sc_uint<64> hi_hybrid  = 0;

    void sample();

    void report();

    SC_CTOR(CreditDutyMon){
        SC_METHOD(sample);
        sensitive << clk.pos();
        dont_initialize();
    }
};


#endif // MODULES_H