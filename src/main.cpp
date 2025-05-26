#include <systemc.h>
#include <iomanip>
#include <queue>  // For std::queue
#include <iostream>  // For std::cout

// Global control for FIFO popping
bool g_enable_popping = false;

// Simple Raw TLP packet
struct RawTLP {
    sc_uint<32>   data;       // 32-bit data field
    sc_uint<2>    thread_id;  // thread identifier (0-2)
    
    // Equality operator for comparing RawTLP objects
    bool operator==(const RawTLP& other) const {
        return (data == other.data) && (thread_id == other.thread_id);
    }
};

// Output stream operator for RawTLP
inline std::ostream& operator<<(std::ostream& os, const RawTLP& tlp) {
    os << "RawTLP(data=" << tlp.data << ", thread_id=" << tlp.thread_id << ")";
    return os;
}

// Custom tracing function for RawTLP
void sc_trace(sc_trace_file* tf, const RawTLP& tlp, const std::string& name) {
    sc_trace(tf, tlp.data, name + ".data");
    sc_trace(tf, tlp.thread_id, name + ".thread_id");
}

SC_MODULE(Threaded_Queue) {
    sc_in<bool> clk;
    sc_in<bool> reset_n;
    sc_in<RawTLP> raw_tlp_in;
    sc_in<bool> valid_in;
    sc_out<bool> credit_out;

    static const unsigned int FIFO_CAPACITY = 8;
    sc_fifo<RawTLP> fifo;
    
    // Credit state registers
    unsigned int credits_current;
    unsigned int credits_next;
    
    // Credit update flags
    bool credit_inc;
    bool credit_dec;

    void process_thread() {
        credit_out.write(false);
        credits_current = 0;
        credits_next = 0;
        credit_inc = false;
        credit_dec = false;

        while (true) {
            wait(); // clock edge

            if (!reset_n.read()) {
                RawTLP dummy;
                while (fifo.nb_read(dummy)) {}  // Clear the FIFO
                credits_current = 0;
                credits_next = 0;
                credit_inc = false;
                credit_dec = false;
                credit_out.write(false);
                continue;
            }

            // Enqueue packet if valid and space available
            if (valid_in.read() && fifo.num_free() > 0) {
                RawTLP pkt = raw_tlp_in.read();
                fifo.write(pkt);
            }

            // Issue credit if space available and credits not maxed out
            if (credits_current < FIFO_CAPACITY && fifo.num_free() > 0) {
                credit_inc = true;
                credits_next = credits_current + 1;
                std::cout << sc_time_stamp() 
                          << " Process thread requesting credit increment - Current=" << credits_current 
                          << " Next=" << credits_next 
                          << std::endl;
                credit_out.write(true);
                wait();  // credit pulse width = 1 clock cycle
                credit_out.write(false);
                credit_inc = false;
            }
        }
    }

    void popper_thread() {
        unsigned int pop_counter = 0;  // Counter for deterministic popping
        
        while (true) {
            wait(); // clock edge

            if (!reset_n.read()) {
                pop_counter = 0;
                continue;
            }

            // Pop every cycle when enabled and FIFO has data
            if (g_enable_popping && fifo.num_available() > 0) {
                if (pop_counter == 3) {  // Pop on every non-zero counter
                    RawTLP pkt = fifo.read();

                    // Decrement credits since space freed
                    if (credits_current > 0) {
                        credit_dec = true;
                        credits_next = credits_current - 1;
                        std::cout << sc_time_stamp() 
                                  << " Popper thread requesting credit decrement - Current=" << credits_current 
                                  << " Next=" << credits_next 
                                  << std::endl;
                    }
                }
                pop_counter = (pop_counter + 1) % 4;  // Cycle counter 0-3
            }
        }
    }

    void credit_update() {
        if (!reset_n.read()) {
            credits_current = 0;
            credits_next = 0;
            credit_inc = false;
            credit_dec = false;
        } else {
            // Update credit state
            if (credit_inc && !credit_dec) {
                credits_current = credits_next;
            } else if (!credit_inc && credit_dec) {
                credits_current = credits_next;
            } else if (credit_inc && credit_dec) {
                std::cout << sc_time_stamp() 
                          << " Credit contention detected - Current=" << credits_current 
                          << " Next=" << credits_next 
                          << " (Process wants to inc, Popper wants to dec)" 
                          << std::endl;
            }
            // Reset flags
            credit_inc = false;
            credit_dec = false;
        }
    }

    SC_CTOR(Threaded_Queue) : fifo(FIFO_CAPACITY) {
        SC_THREAD(process_thread);
        sensitive << clk.pos();

        SC_THREAD(popper_thread);
        sensitive << clk.pos();
        
        // Add credit update method
        SC_METHOD(credit_update);
        sensitive << clk.pos() << reset_n;
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
        }
    }

    // Thread for sending TLPs
    void sender_thread() {
        // Initialize
        packet_seq = 1;
        raw_valid.write(false);
        
        while (true) {
            // Reset valid signal by default
            raw_valid.write(false);
            
            if (!reset_n.read()) {
                // Reset state
                packet_seq = 1;
            } else {
                // Try to send a packet if we have credits for any thread
                // Round-robin between threads with available credits
                static int current_thread = 1;  // Start from thread 1
                
                // Try each thread starting from current_thread
                for (int i = 0; i < 3; i++) {
                    int thread_to_try = ((current_thread - 1 + i) % 3) + 1;  // Map 0-2 to 1-3
                    
                    if (credit_counter[thread_to_try - 1] > 0) {  // Adjust index for credit_counter
                        // Create and send a packet for this thread
                        RawTLP pkt;
                        pkt.data = packet_seq;
                        pkt.thread_id = thread_to_try;
                        
                        raw_tlp.write(pkt);
                        raw_valid.write(true);
                        
                        // Consume one credit for this thread
                        credit_counter[thread_to_try - 1]--;  // Adjust index for credit_counter
                        
                        // Increment packet sequence
                        packet_seq++;
                        
                        // Update round-robin pointer
                        current_thread = thread_to_try + 1;
                        if (current_thread > 3) current_thread = 1;
                        
                        // We've sent a packet, exit the loop
                        break;
                    }
                }
            }
            
            // Wait for next clock edge or credit event
            wait(clk.posedge_event() | credit_event);
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

    void credit_combine_thread() {
        while (true) {
            wait(clk.posedge_event());
            sc_uint<3> combined_credits = 0;
            for (int i = 0; i < 3; i++) {
                combined_credits[i] = credit_signals[i].read();
            }
            credit_out.write(combined_credits);
        }
    }

    SC_CTOR(iEP) {
        for (int i = 0; i < 3; i++) {
            threaded_queues[i] = new Threaded_Queue(sc_gen_unique_name("threaded_queue"));
            threaded_queues[i]->clk(clk);
            threaded_queues[i]->reset_n(reset_n);
            threaded_queues[i]->raw_tlp_in(tlp_signals[i]);
            threaded_queues[i]->valid_in(raw_valid);
            threaded_queues[i]->credit_out(credit_signals[i]);
        }

        SC_METHOD(input_router);
        sensitive << clk.pos() << reset_n << raw_valid << raw_tlp;
        
        SC_THREAD(credit_combine_thread);
        sensitive << clk.pos() << reset_n << credit_signals[0] << credit_signals[1] << credit_signals[2];
    }

    void input_router() {
        if (raw_valid.read()) {
            RawTLP pkt = raw_tlp.read();
            unsigned int thread_id = pkt.thread_id.to_uint();
            if (thread_id >= 1 && thread_id <= 3) {  // Check for threads 1-3
                tlp_signals[thread_id - 1].write(pkt);  // Adjust index for tlp_signals
            }
        }
    }
};

// Top-level sc_main
int sc_main(int argc, char* argv[]) {
    sc_clock           system_clk("system_clk", 100, SC_NS);  // Single common clock
    sc_signal<bool>    reset_n;
    sc_signal<sc_uint<3>> credit;  // 3-bit credit bus
    sc_signal<bool>    raw_valid;
    sc_signal<RawTLP>  raw_tlp;

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

    sc_trace_file* tf = sc_create_vcd_trace_file("irc_iep_flow");
    tf->set_time_unit(1, SC_NS);
    sc_trace(tf, system_clk,         "system_clk");  // Trace the common clock
    sc_trace(tf, reset_n,            "reset_n");
    sc_trace(tf, raw_valid,          "raw_valid");
    sc_trace(tf, raw_tlp,            "raw_tlp");  // This will now use our custom tracing function
    sc_trace(tf, credit,             "credit");

    // Initial values - just initialize reset
    reset_n.write(false);
    
    // Apply reset for 20ns
    sc_start(20, SC_NS);
    reset_n.write(true);
    
    // Run the simulation for longer to see the effects
    sc_start(20, SC_US);
    sc_close_vcd_trace_file(tf);
    return 0;
}

