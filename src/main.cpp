#include <systemc.h>
#include <iomanip>
#include <queue>  // For std::queue
#include <iostream>  // For std::cout

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

SC_MODULE(Threaded_Queue) {
    sc_in<bool> clk;
    sc_in<bool> reset_n;
    sc_in<RawTLP> raw_tlp_in;
    sc_in<bool> valid_in;
    sc_out<bool> credit_out;

    static const unsigned int FIFO_CAPACITY = 8;
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

// iRC: Root Complex module (sender)
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
            wait(1, SC_NS);
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
                    std::cout << "[iRC] Send seq="<<pkt.seq_num<<" tid="<<pkt.thread_id << std::endl;

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

    Threaded_Queue* threaded_queues[3];
    sc_signal<bool> credit_signals[3];
    sc_signal<RawTLP> tlp_signals[3];
    sc_signal<bool>   valid_signals[3];

    // Method to process popped data
    void process_popped_data(const RawTLP& pkt, int queue_id) {
        // TODO: Add your custom processing logic here
        std::cout << sc_time_stamp() 
                  << " Processing popped packet from queue " << queue_id 
                  << " with seq_num=" << pkt.seq_num 
                  << " thread_id=" << pkt.thread_id 
                  << std::endl;
    }

    void credit_combine_thread() {
        while (true) {
            wait(clk.posedge_event());
            // Ensure data driven by iRC in the same clock phase is visible
            wait(SC_ZERO_TIME);
            sc_uint<3> combined_credits = 0;
            for (int i = 0; i < 3; i++) {
                combined_credits[i] = credit_signals[i].read();
            }
            credit_out.write(combined_credits);
        }
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
                        if (threaded_queues[i]->pop_data(pkt)) {
                            process_popped_data(pkt, i);
                        }
                    }
                }
                pop_counter = (pop_counter + 1) % 4;  // Cycle counter 0-3
            }
        }
    }

    SC_CTOR(iEP) {
        for (int i = 0; i < 3; i++) {
            std::string queue_name = std::string(name()) + "_queue_" + std::to_string(i);
            threaded_queues[i] = new Threaded_Queue(queue_name.c_str());
            threaded_queues[i]->clk(clk);
            threaded_queues[i]->reset_n(reset_n);
            threaded_queues[i]->raw_tlp_in(tlp_signals[i]);
            threaded_queues[i]->valid_in(valid_signals[i]);
            threaded_queues[i]->credit_out(credit_signals[i]);
        }

        SC_THREAD(input_router_thread);
        sensitive << clk.pos();
        
        SC_THREAD(credit_combine_thread);
        sensitive << clk.pos() << reset_n << credit_signals[0] << credit_signals[1] << credit_signals[2];

        SC_THREAD(popper_thread);
        sensitive << clk.pos() << reset_n;
    }

    void input_router_thread() {
        while (true) {
            wait(clk.posedge_event());

            if (!reset_n.read()) {
                continue;
            }

            wait(SC_ZERO_TIME);

            // Default deassert
            for(int i=0;i<3;++i) valid_signals[i].write(false);

            if (raw_valid.read()) {
                RawTLP pkt = raw_tlp.read();
                unsigned int tid = pkt.thread_id.to_uint();
                if (tid >= 1 && tid <= 3) {
                    tlp_signals[tid - 1].write(pkt);
                    valid_signals[tid - 1].write(true); // one-delta pulse
                    debug_route(name(), pkt, tid - 1);
                }
            }
        }
    }
};

// TX: Transmit module with valid/ready handshaking
SC_MODULE(TX) {
    sc_in<bool>         clk;
    sc_in<bool>         reset_n;
    sc_in<bool>         ingress_valid;
    sc_in<RawTLP>       ingress_tlp;
    sc_out<sc_uint<3>>  credit_out;

    // Output interface for next module
    sc_out<bool>        egress_valid;
    sc_out<RawTLP>      egress_tlp;
    sc_in<bool>         egress_ready;

    Threaded_Queue* threaded_queues[3];
    sc_signal<bool> credit_signals[3];
    sc_signal<RawTLP> tlp_signals[3];
    sc_signal<bool>   valid_signals[3];

    void credit_combine_thread() {
        while (true) {
            wait(clk.posedge_event());
            // Ensure data driven by iRC in the same clock phase is visible
            wait(SC_ZERO_TIME);
            sc_uint<3> combined_credits = 0;
            for (int i = 0; i < 3; i++) {
                combined_credits[i] = credit_signals[i].read();
            }
            credit_out.write(combined_credits);
        }
    }

    void transmitter_thread() {
        wait(SC_ZERO_TIME);  // Initial wait for proper synchronization

        // State for holding a packet across cycles until handshake completes
        bool     holding = false;
        RawTLP   held_pkt;
        int      current_queue = 0; // round-robin pointer

        while (true) {
            wait(clk.posedge_event());

            if (!reset_n.read()) {
                egress_valid.write(false);
                holding = false;
                continue;
            }

            wait(SC_ZERO_TIME);

            // If we are not currently holding a packet, try to fetch one
            if (!holding) {
                for (int i = 0; i < 3; ++i) {
                    int q = (current_queue + i) % 3;
                    if (threaded_queues[q]->has_data()) {
                        if (threaded_queues[q]->pop_data(held_pkt)) {
                            holding = true;
                            std::cout << sc_time_stamp() << " [TX] Hold pkt seq=" << held_pkt.seq_num << " tid=" << held_pkt.thread_id << " from queue " << q << std::endl;
                            current_queue = (q + 1) % 3;  // advance pointer
                            break;
                        }
                    }
                }
            }

            // Drive outputs according to holding state
            if (holding) {
                egress_tlp.write(held_pkt);
                egress_valid.write(true);
            } else {
                egress_valid.write(false);
            }

            // If handshake completed, drop the held packet on next cycle
            if (holding && egress_ready.read()) {
                std::cout << sc_time_stamp() << " [TX] RX accepted seq=" << held_pkt.seq_num << std::endl;
                holding = false;  // packet consumed by RX
            }
        }
    }

    SC_CTOR(TX) {
        for (int i = 0; i < 3; i++) {
            std::string queue_name = std::string(name()) + "_queue_" + std::to_string(i);
            threaded_queues[i] = new Threaded_Queue(queue_name.c_str());
            threaded_queues[i]->clk(clk);
            threaded_queues[i]->reset_n(reset_n);
            threaded_queues[i]->raw_tlp_in(tlp_signals[i]);
            threaded_queues[i]->valid_in(valid_signals[i]);
            threaded_queues[i]->credit_out(credit_signals[i]);
        }

        SC_THREAD(input_router_thread);
        sensitive << clk.pos();
        
        SC_THREAD(credit_combine_thread);
        sensitive << clk.pos() << reset_n << credit_signals[0] << credit_signals[1] << credit_signals[2];

        SC_THREAD(transmitter_thread);
        sensitive << clk.pos() << reset_n << egress_ready;
    }

    void input_router_thread() {
        while (true) {
            wait(clk.posedge_event());

            if (!reset_n.read()) {
                continue;
            }

            wait(SC_ZERO_TIME);

            // Default deassert
            for(int i=0;i<3;++i) valid_signals[i].write(false);

            if (ingress_valid.read()) {
                RawTLP pkt = ingress_tlp.read();
                unsigned int tid = pkt.thread_id.to_uint();
                if (tid >= 1 && tid <= 3) {
                    tlp_signals[tid - 1].write(pkt);
                    valid_signals[tid - 1].write(true); // one-delta pulse
                    debug_route(name(), pkt, tid - 1);
                }
            }
        }
    }
};

// RX: Receive module with large FIFO
SC_MODULE(RX) {
    sc_in<bool>         clk;
    sc_in<bool>         reset_n;
    sc_in<bool>         valid_in;
    sc_in<RawTLP>       tlp_in;
    sc_out<bool>        ready_out;

    static const unsigned int RX_FIFO_CAPACITY = 1024;  // Large FIFO
    sc_fifo<RawTLP> fifo;

    void receiver_thread() {
        while (true) {
            wait(clk.posedge_event());

            if (!reset_n.read()) {
                ready_out.write(false);
                continue;
            }

            // Only ready to accept data if FIFO has space and popping is enabled
            ready_out.write(fifo.num_free() > 0 && g_enable_popping);

            // Accept data if valid and FIFO has space
            if (valid_in.read() && fifo.num_free() > 0) {
                RawTLP pkt = tlp_in.read();
                fifo.write(pkt);
                std::cout << sc_time_stamp() << " RX: Received seq_num=" << pkt.seq_num << " tid=" << pkt.thread_id << " (occ=" << fifo.num_available() << "/" << RX_FIFO_CAPACITY << ")" << std::endl;
            }
            wait(1, SC_NS);
        }
    }

    SC_CTOR(RX) : fifo(RX_FIFO_CAPACITY) {
        SC_THREAD(receiver_thread);
        sensitive << clk.pos() << reset_n << valid_in << tlp_in;
    }
};

// Top-level sc_main
int sc_main(int argc, char* argv[]) {
    // Scan cmd-line for run-time trace flag
    for(int i=1;i<argc;++i){
#ifdef PACKET_TRACE
        if(std::string(argv[i])=="--trace-off") g_pkt_trace=false;
#endif
    }

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

    // Signals for TX topology
    sc_signal<sc_uint<3>> credit_tx2iRC;
    sc_signal<bool>    raw_valid_iRC2TX;
    sc_signal<RawTLP>  raw_tlp_iRC2TX;
    sc_signal<bool>    TX2RX_valid;
    sc_signal<RawTLP>  TX2RX_tlp;
    sc_signal<bool>    RX2TX_ready;

    // Create and connect TX topology
    iRC rc_tx("iRC_tx");
    rc_tx.clk(system_clk);
    rc_tx.reset_n(reset_n);
    rc_tx.credit_in(credit_tx2iRC);
    rc_tx.raw_valid(raw_valid_iRC2TX);
    rc_tx.raw_tlp(raw_tlp_iRC2TX);

    TX tx("TX");
    tx.clk(system_clk);
    tx.reset_n(reset_n);
    tx.ingress_valid(raw_valid_iRC2TX);
    tx.ingress_tlp(raw_tlp_iRC2TX);
    tx.credit_out(credit_tx2iRC);
    tx.egress_valid(TX2RX_valid);
    tx.egress_tlp(TX2RX_tlp);
    tx.egress_ready(RX2TX_ready);

    // Create and connect RX module
    RX rx("RX");
    rx.clk(system_clk);
    rx.reset_n(reset_n);
    rx.valid_in(TX2RX_valid);
    rx.tlp_in(TX2RX_tlp);
    rx.ready_out(RX2TX_ready);

    // Trace TX topology signals
    sc_trace(tf_tx, system_clk, "system_clk");
    sc_trace(tf_tx, reset_n, "reset_n");
    sc_trace(tf_tx, raw_valid_iRC2TX, "raw_valid_iRC2TX");
    sc_trace(tf_tx, raw_tlp_iRC2TX, "raw_tlp_iRC2TX");
    sc_trace(tf_tx, credit_tx2iRC, "credit_tx2iRC");
    sc_trace(tf_tx, TX2RX_valid, "TX2RX_valid");
    sc_trace(tf_tx, TX2RX_tlp, "TX2RX_tlp");
    sc_trace(tf_tx, RX2TX_ready, "RX2TX_ready");

    // Initial values
    reset_n.write(false);
    
    // Apply reset for 20ns
    sc_start(20, SC_NS);
    reset_n.write(true);
    
    // Run the simulation for longer to see the effects
    sc_start(20, SC_US);

    // Close trace files
    sc_close_vcd_trace_file(tf);
    sc_close_vcd_trace_file(tf_tx);

    return 0;
}

