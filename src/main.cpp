#include <systemc.h>
#include <iomanip>
#include <queue>  // For std::queue
#include <iostream>  // For std::cout
#include <string>
#include <algorithm>
#include <cstdlib>

// -----------------------------------------------------------------------------
// Central build-time configuration (all sizes / latencies in one place)
// -----------------------------------------------------------------------------

constexpr unsigned TX_FIFO_DEPTH   = 1024;   // entries (24 packets @ 64-bit)
constexpr unsigned RX_FIFO_DEPTH   = 24;   // entries
constexpr unsigned THREAD_Q_DEPTH = 8;   // per-thread depth inside iEP/FrontEnd
const unsigned GLOBAL_SENSE_WINDOW = 8; // can be tuned – equal to Threaded FIFO depth

constexpr unsigned NOC_STATIC_LATENCY = 12;  // fixed AXI cycles through NoC
constexpr unsigned NOC_STALL_PCT  = 25; // percentage (0-99) of cycles ready is LOW
constexpr unsigned NOC_PATTERN_LEN = 100; // resolution (cycles)
static_assert(NOC_STALL_PCT < 100, "stall percentage must be <100");

// Global control for FIFO popping
bool g_enable_popping = true;

// Simple Raw TLP packet
struct RawTLP {
    sc_uint<32>   seq_num;    // Unique sequence number
    sc_uint<2>    thread_id;  // thread identifier (0-2)
    
    // Equality operator for comparing RawTLP objects
    bool operator==(const RawTLP& other) const {
        return (seq_num == other.seq_num) && (thread_id == other.thread_id);
    }
};

// Output stream operator for RawTLP
inline std::ostream& operator<<(std::ostream& os, const RawTLP& tlp) {
    os << "RawTLP(seq_num=" << tlp.seq_num << ", thread_id=" << tlp.thread_id << ")";
    return os;
}

// Custom tracing function for RawTLP
void sc_trace(sc_trace_file* tf, const RawTLP& tlp, const std::string& name) {
    sc_trace(tf, tlp.seq_num, name + ".seq_num");
    sc_trace(tf, tlp.thread_id, name + ".thread_id");
}

// Helper debug print for routing actions
inline void debug_route(const char* mod, const RawTLP& pkt, int q) {
    std::cout << "["<<mod<<"] Route: seq_num="<<pkt.seq_num<<" tid="<<pkt.thread_id<<" -> queue "<<q << std::endl;
}

// ---------------- AXI Stream word used only between TX and RX ----------------
struct AxiWord {
    sc_uint<64> data{0};
    bool        tlast{true};

    bool operator==(const AxiWord& o) const { return data == o.data && tlast == o.tlast; }
};

inline std::ostream& operator<<(std::ostream& os, const AxiWord& w){
    os << "AxiWord(data=" << w.data << ", tlast=" << w.tlast << ")"; return os;
}

inline void sc_trace(sc_trace_file* tf, const AxiWord& w, const std::string& n){
    sc_trace(tf, w.data,   n+".data");
    sc_trace(tf, w.tlast,  n+".tlast");
}

// -----------------------------------------------------------------------------
// Conversion helpers used by TX/RX and any future network elements
// -----------------------------------------------------------------------------

inline AxiWord tlp_to_axi(const RawTLP& p){
    AxiWord w; w.tlast = true;
    sc_uint<64> d = p.seq_num;
    d.range(33,32) = p.thread_id;
    w.data = d;
    return w;
}

inline RawTLP axi_to_tlp(const AxiWord& w){
    RawTLP p; p.seq_num = w.data.range(31,0);
    p.thread_id = w.data.range(33,32);
    return p;
}

// -----------------------------------------------------------------------------
// Helper for packing three 10-bit credit counters into a single AXI beat

inline AxiWord credits_to_axi(const sc_uint<16> c0,
                              const sc_uint<16> c1,
                              const sc_uint<16> c2){
    AxiWord w; w.tlast = true;
    sc_uint<64> d = 0;
    d.range(15,0)   = c0;
    d.range(31,16)  = c1;
    d.range(47,32)  = c2;
    w.data = d;
    return w;
}

inline void axi_to_credits(const AxiWord& w,
                           sc_uint<16>& c0,
                           sc_uint<16>& c1,
                           sc_uint<16>& c2){
    sc_uint<64> d = w.data;
    c0 = d.range(15,0);
    c1 = d.range(31,16);
    c2 = d.range(47,32);
}

// -----------------------------------------------------------------------------

SC_MODULE(Threaded_Queue) {
    sc_in<bool> clk;
    sc_in<bool> reset_n;
    sc_in<RawTLP> raw_tlp_in;
    sc_in<bool> valid_in;
    sc_out<bool> credit_out;

    static const unsigned int FIFO_CAPACITY = THREAD_Q_DEPTH;
    sc_fifo<RawTLP> fifo;
    
    // Credit state
    unsigned int credits;
    bool credit_pending;

    void main_thread() {
        wait(SC_ZERO_TIME);  // Initial wait for proper synchronization
        
        // Initialize
        credits = 0;
        credit_pending = false;
        credit_out.write(false);

        while (true) {
            wait(clk.posedge_event());
            // Allow router to update tlp_signals this cycle
            wait(SC_ZERO_TIME);
            
            if (!reset_n.read()) {
                // Reset state
                RawTLP dummy;
                while (fifo.nb_read(dummy)) {}  // Clear the FIFO
                credits = 0;
                credit_pending = false;
                credit_out.write(false);
                continue;
            }

            // Handle credit updates first
            if (credit_pending) {
                credit_out.write(false);
                credit_pending = false;
            }

            // Enqueue packet if valid and space available
            if (valid_in.read() && fifo.num_free() > 0) {
                RawTLP pkt = raw_tlp_in.read();
                fifo.write(pkt);
                std::cout << "["<<name()<<"] Enqueue: seq_num="<<pkt.seq_num<<" thread_id="<<pkt.thread_id<<" (FIFO occ="<<fifo.num_available()<<")" << std::endl;
            }

            // Issue credit if space available and credits not maxed out
            if (credits < FIFO_CAPACITY && fifo.num_free() > 0) {
                credits++;
                credit_out.write(true);
                credit_pending = true;
                std::cout << sc_time_stamp() 
                          << " [" << name() << "] Issuing credit - Current=" << credits 
                          << std::endl;
            }
        }
    }

    // Method to check if data is available
    bool has_data() const {
        return fifo.num_available() > 0;
    }

    // Method to pop data from FIFO
    bool pop_data(RawTLP& pkt) {
        if (fifo.nb_read(pkt)) {  // Use non-blocking read
            if (credits > 0) {
                credits--;
                std::cout << sc_time_stamp() 
                          << " [" << name() << "] Popping data: seq_num=" << pkt.seq_num
                          << ", thread_id=" << pkt.thread_id
                          << " - Credits=" << credits 
                          << std::endl;
            }
            return true;
        }
        return false;
    }

    SC_CTOR(Threaded_Queue) : fifo(FIFO_CAPACITY) {
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
    bool has_data(int idx) const {
        return queues[idx]->has_data();
    }

    bool pop_data(int idx, RawTLP& pkt) {
        return queues[idx]->pop_data(pkt);
    }

    // Router thread: demux ingress packet by thread_id -> per-queue signals
    void input_router_thread() {
        while (true) {
            wait(clk.posedge_event());

            if (!reset_n.read()) {
                // de-assert all valids when in reset
                for (int i = 0; i < 3; ++i) valid_signals[i].write(false);
                continue;
            }

            wait(SC_ZERO_TIME); // 1-delta to allow producer observations

            for (int i = 0; i < 3; ++i) valid_signals[i].write(false);

            if (ingress_valid.read()) {
                RawTLP pkt = ingress_tlp.read();
                unsigned tid = pkt.thread_id.to_uint();
                if (tid >= 1 && tid <= 3) {
                    tlp_signals[tid - 1].write(pkt);
                    valid_signals[tid - 1].write(true);
                    debug_route(name(), pkt, tid - 1);
                }
            }
        }
    }

    // Combiner: OR-reduce per-queue credit pulses into a 3-bit bus
    void credit_combine_thread() {
        while (true) {
            wait(clk.posedge_event());
            wait(SC_ZERO_TIME);
            sc_uint<3> combined = 0;
            for (int i = 0; i < 3; ++i) combined[i] = credit_signals[i].read();
            credit_out.write(combined);
        }
    }

    SC_CTOR(ThreadedFrontEnd) {
        // Build child queue names without illegal '.' characters to avoid SystemC W506.
        std::string prefix(name());
        std::replace(prefix.begin(), prefix.end(), '.', '_');
        for (int i = 0; i < 3; ++i) {
            std::string qn = prefix + "_queue_" + std::to_string(i);
            queues[i] = new Threaded_Queue(qn.c_str());
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

    void main_thread() {
        wait(SC_ZERO_TIME);
        bool holding = false;
        RawTLP held_pkt;
        while (true) {
            wait(clk.posedge_event());

            // enqueue from iRC_tx
            if (ingress_valid.read() && fifo.num_free() > 0) {
                fifo.write(ingress_tlp.read());
            }

            // track occupancy
            unsigned cur_occ = fifo.num_available() + (holding ? 1 : 0);
            if (cur_occ > max_occ) {
                max_occ = cur_occ;
                std::cout << sc_time_stamp()
                          << " [TX_FIFO] depth=" << max_occ << std::endl;
            }

            // fetch new packet when current TLP fully sent
            if (!holding && fifo.nb_read(held_pkt)) {
                holding = true;
            }

            // drive outputs
            if (holding) {
                AxiWord w = tlp_to_axi(held_pkt);
                egress_axi.write(w);
                egress_valid.write(true);

                if (egress_ready.read()) {
                    holding = false;
                }
            } else {
                egress_valid.write(false);
            }
        }
    }

    SC_CTOR(SimpleTxFIFO, unsigned depth=TX_FIFO_DEPTH) : fifo(depth) {
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

    void main_thread() {
        wait(SC_ZERO_TIME);
        while (true) {
            wait(clk.posedge_event());

            ready_out.write(fifo.num_free() > 0);

            unsigned int occ = fifo.num_available();
            if (occ > max_occ) { max_occ = occ; std::cout << sc_time_stamp() << " [RX_FIFO] depth=" << max_occ << std::endl; }

            if (valid_in.read() && ready_out.read()) {
                AxiWord aw = axi_in.read();
                fifo.write(axi_to_tlp(aw));
            }

            RawTLP pkt;
            if (fifo.nb_read(pkt)) {
                tlp_out.write(pkt);
                valid_out.write(true);
            } else {
                valid_out.write(false);
            }
        }
    }

    SC_CTOR(SimpleRxFIFO, unsigned depth=RX_FIFO_DEPTH) : fifo(depth) {
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

    void main_thread(){
        wait(SC_ZERO_TIME);
        while(true){
            wait(clk.posedge_event());

            if(!reset_n.read()){
                for(int i=0;i<3;++i) accum[i]=0;
                ctr = 0; sending = false; valid_out.write(false);
                continue;
            }

            // Default deassert
            if(!sending) valid_out.write(false);

            // count incoming credit pulses every cycle
            sc_uint<3> in = credit_in.read();
            for(int i=0;i<3;++i) if(in[i]) accum[i]++;

            // When not currently sending, check window expiry
            if(!sending){
                if(++ctr == GLOBAL_SENSE_WINDOW){
                    ctr = 0;
                    // Build packet even if some counts are zero – optional
                    pending = credits_to_axi(accum[0],accum[1],accum[2]);
                    for(int i=0;i<3;++i) accum[i]=0;
                    sending = true;
                    valid_out.write(true);
                    axi_out.write(pending);
                }
            } else { // currently asserting valid
                if(ready_in.read()){
                    sending = false;          // beat accepted
                    valid_out.write(false);
                } else {
                    // keep driving same word
                    valid_out.write(true);
                    axi_out.write(pending);
                }
            }
        }
    }

    SC_CTOR(CreditTx){
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

    void main_thread(){
        wait(SC_ZERO_TIME);
        while(true){
            wait(clk.posedge_event());

            if(!reset_n.read()){
                for(int i=0;i<3;++i) emit_cnt[i]=0;
                ready_out.write(true);
                credit_out.write(0);
                continue;
            }

            // Default outputs
            credit_out.write(0);

            bool empty = (emit_cnt[0]==0 && emit_cnt[1]==0 && emit_cnt[2]==0);
            ready_out.write(empty); // accept new packet only when previous drained

            // Emit phase
            if(!empty){
                sc_uint<3> pulse = 0;
            for(int i=0;i<3;++i){
                    if(emit_cnt[i]!=0){
                        pulse[i]=1;
                        emit_cnt[i]--;
                    }
                }
                credit_out.write(pulse);
            }

            // Acceptance of new packet
            if(valid_in.read() && ready_out.read()){
                sc_uint<16> cnt0,cnt1,cnt2; axi_to_credits(axi_in.read(),cnt0,cnt1,cnt2);
                emit_cnt[0]=cnt0;
                emit_cnt[1]=cnt1;
                emit_cnt[2]=cnt2;
            }
        }
    }

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
    void credit_monitor_thread() {
        // Initialize credit counters
        for (int i = 0; i < 3; i++) {
            credit_counter[i] = 0;
        }
        
        while (true) {
            wait(clk.posedge_event());
            
            if (!reset_n.read()) {
                // Reset state
                for (int i = 0; i < 3; i++) {
                    credit_counter[i] = 0;
                }
            } else {
                // Sample credit pulse on any bit
                sc_uint<3> credits = credit_in.read();
                bool credit_received = false;
                
                for (int i = 0; i < 3; i++) {
                    if (credits[i]) {
                        // Credit pulse received for thread i+1 (since threads are 1-3)
                        credit_counter[i]++;
                        credit_received = true;
                    }
                }
                
                if (credit_received) {
                    credit_event.notify();
                }
            }
        }
    }

    // Thread for sending TLPs
    void sender_thread() {
        // Initialize
        packet_seq = 1;
        raw_valid.write(false);
        
        static int current_thread = 1;  // Persistent round-robin pointer

        while (true) {
            wait(clk.posedge_event());  // Main clock-driven loop

            if (!reset_n.read()) {
                // Reset state
                packet_seq = 1;
                raw_valid.write(false);
                continue;
            }

            // By default, deassert valid every cycle
            raw_valid.write(false);

            // Try to send a packet if we have credits for any thread
            for (int i = 0; i < 3; i++) {
                int thread_to_try = ((current_thread - 1 + i) % 3) + 1;  // Map 0-2 to 1-3

                if (credit_counter[thread_to_try - 1] > 0) {  // Adjust index for credit_counter
                    // Create and send a packet for this thread
                    RawTLP pkt;
                    pkt.seq_num = packet_seq;
                    pkt.thread_id = thread_to_try;

                    raw_tlp.write(pkt);
                    raw_valid.write(true);   // Assert valid for this cycle only
                    std::cout << sc_time_stamp() << " [" << name() << "] Send seq="<<pkt.seq_num<<" tid="<<pkt.thread_id << std::endl;

                    // Consume one credit for this thread
                    credit_counter[thread_to_try - 1]--;  // Adjust index for credit_counter

                    // Increment packet sequence
                    packet_seq++;

                    // Update round-robin pointer to the next thread
                    current_thread = thread_to_try + 1;
                    if (current_thread > 3) current_thread = 1;

                    break;  // Only send one packet per cycle
                }
            }
        }
    }

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
    void process_popped_data(const RawTLP& pkt, int queue_id) {
        // TODO: Add your custom processing logic here
        std::cout << sc_time_stamp() 
                  << " Processing popped packet from queue " << queue_id 
                  << " with seq_num=" << pkt.seq_num 
                  << " thread_id=" << pkt.thread_id 
                  << std::endl;
    }

    void popper_thread() {
        unsigned int pop_counter = 0;  // Counter for deterministic popping
        
        while (true) {
            wait(clk.posedge_event());

            if (!reset_n.read()) {
                pop_counter = 0;
                continue;
            }

            // Pop from queues when enabled
            if (g_enable_popping) {
                std::cout << sc_time_stamp() << " [iEP popper] counter=" << pop_counter << std::endl;
                if (pop_counter == 3) {  // Pop on every 4th cycle
                    // Try to pop from each queue
                    for (int i = 0; i < 3; i++) {
                        RawTLP pkt;
                        if (threaded_queues->pop_data(i, pkt)) {
                            process_popped_data(pkt, i);
                        }
                    }
                }
                pop_counter = (pop_counter + 1) % 4;  // Cycle counter 0-3
            }
        }
    }

    SC_CTOR(iEP) {
        threaded_queues = new ThreadedFrontEnd((std::string(name()) + "_front").c_str());
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
    sc_in<bool>      clk;
    sc_in<bool>      reset_n;
    // upstream (TX) side
    sc_in<bool>      valid_in;
    sc_in<AxiWord>   axi_in;
    sc_out<bool>     ready_out;  // back to TX
    // downstream (RX) side
    sc_out<bool>     valid_out;
    sc_out<AxiWord>  axi_out;
    sc_in<bool>      ready_in;

    // fixed pipeline latency buffer & back-pressure generation
    static const unsigned PIPE_LAT = NOC_STATIC_LATENCY; // latency cycles
    AxiWord  pipe[PIPE_LAT];
    bool     pipe_valid[PIPE_LAT] = {false};
    unsigned pattern_ctr = 0;          // 0..NOC_PATTERN_LEN-1

    void main_thread(){
            wait(SC_ZERO_TIME);
        while(true){
            wait(clk.posedge_event());

            // Deterministic duty-cycle stall: ready low for NOC_STALL_PCT % of pattern len
            const unsigned stall_cycles = (NOC_PATTERN_LEN * NOC_STALL_PCT) / 100;
            bool stall_active = (pattern_ctr < stall_cycles);
            bool ready_ok = !pipe_valid[0] && !stall_active;
            ready_out.write(ready_ok);

            if(valid_in.read() && ready_ok){
                pipe[0] = axi_in.read();
                pipe_valid[0] = true;

                // nothing special to do; stall pattern is time-based
            }

            pattern_ctr = (pattern_ctr + 1) % NOC_PATTERN_LEN;

            // Drive output when last stage valid
            if(pipe_valid[PIPE_LAT-1]){
                valid_out.write(true);
                axi_out.write(pipe[PIPE_LAT-1]);
                if(ready_in.read()) pipe_valid[PIPE_LAT-1] = false;
            } else {
                valid_out.write(false);
            }

            // shift pipeline each clock
            for (int i = PIPE_LAT-1; i > 0; --i) {
                if (!pipe_valid[i] && pipe_valid[i-1]) {
                    pipe[i]       = pipe[i-1];
                    pipe_valid[i] = true;
                    pipe_valid[i-1] = false;
                }
            }
        }
    }

    SC_CTOR(AxiNoC) {
        SC_THREAD(main_thread);
        sensitive<<clk.pos();
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

    void sample(){
        total++;
        if(bus_direct.read() != 0) hi_direct++;
        if(bus_hybrid.read() != 0) hi_hybrid++;
    }

    void report(){
        std::cout << "\n---- Credit bus duty cycle ----\n";
        if(total == 0){
            std::cout << "No samples taken!\n";
            return;
        }
        auto pct = [&](sc_uint<64> hi){ return 100.0 * static_cast<double>(hi) / static_cast<double>(total); };
        std::cout << "Direct bus : "  << pct(hi_direct) << " %\n";
        std::cout << "Hybrid bus : "  << pct(hi_hybrid) << " %\n";
    }

    SC_CTOR(CreditDutyMon){
        SC_METHOD(sample);
        sensitive << clk.pos();
        dont_initialize();
    }
};

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

    iEP ep("iEP");
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
    iEP ep_rx("iEP_after_RX");
    ep_rx.clk(system_clk);
    ep_rx.reset_n(reset_n);
    ep_rx.raw_valid(RX2EP_valid);
    ep_rx.raw_tlp(RX2EP_tlp);
    ep_rx.credit_out(credit_iEP2cTx);

    // Credit path over its own deterministic AXI NoC
    CreditTx c_tx("CreditTx");
    c_tx.clk(system_clk);
    c_tx.reset_n(reset_n);
    c_tx.credit_in(credit_iEP2cTx);
    c_tx.valid_out(c_valid_tx);
    c_tx.axi_out(c_axi_tx);
    c_tx.ready_in(c_ready_tx);

    // Use a separate NoC instance but same behaviour
    AxiNoC c_noc("Credit_NOC");
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
    AxiNoC noc("AXI_NOC");
    noc.clk(system_clk);
    noc.reset_n(reset_n);
    noc.valid_in(TX2NOC_valid);
    noc.axi_in(TX2NOC_axi);
    noc.ready_out(NOC2TX_ready);
    noc.valid_out(NOC2RX_valid);
    noc.axi_out(NOC2RX_axi);
    noc.ready_in(RX2NOC_ready);

    // traces – entire TX→NoC→RX path plus proxy credits
    sc_trace(tf_tx, RC2TX_raw_valid, "RC2TX_raw_valid");
    sc_trace(tf_tx, RC2TX_raw_tlp,  "RC2TX_raw_tlp");
    sc_trace(tf_tx, TX2NOC_valid,    "TX2NOC_valid");
    sc_trace(tf_tx, NOC2TX_ready, "NOC_ready_to_TX");
    sc_trace(tf_tx, TX2NOC_axi,   "TX2NOC_axi");
    sc_trace(tf_tx, NOC2RX_valid,  "NOC2RX_valid");
    sc_trace(tf_tx, NOC2RX_axi,    "NOC2RX_axi");
    sc_trace(tf_tx, RX2NOC_ready,  "RX_ready_to_NOC");
    sc_trace(tf_tx, RX2EP_valid, "RX2EP_valid");
    sc_trace(tf_tx, RX2EP_tlp,   "RX2EP_tlp");
    sc_trace(tf_tx, c_valid_tx, "credit_valid_tx");
    sc_trace(tf_tx, c_ready_tx, "credit_ready_tx");
    sc_trace(tf_tx, c_valid_rx, "credit_valid_rx");
    sc_trace(tf_tx, credit_pkt2rc, "credit_pkt2rc");
    sc_trace(tf_tx, credit_iEP2cTx, "credit_iEP2cTx");

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
    
    // Run the simulation for longer to see the effects
    sc_start(200, SC_US);

    // Print duty cycle stats
    mon.report();

    // Close trace files
    sc_close_vcd_trace_file(tf);
    sc_close_vcd_trace_file(tf_tx);

    return 0;
}

