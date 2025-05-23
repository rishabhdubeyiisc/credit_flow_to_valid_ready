#include <systemc.h>
#include <iomanip>
#include <queue>  // For std::queue

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
            // Only sample credits on positive clock edge to prevent multiple detections
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
                        // Credit pulse received for thread i
                        credit_counter[i]++;
                        credit_received = true;
                    }
                }
                
                if (credit_received) {
                    credit_event.notify(); // Notify the sender thread
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
                static int current_thread = 0;
                
                // Try each thread starting from current_thread
                for (int i = 0; i < 3; i++) {
                    int thread_to_try = (current_thread + i) % 3;
                    
                    if (credit_counter[thread_to_try] > 0) {
                        // Create and send a packet for this thread
                        RawTLP pkt;
                        pkt.data = packet_seq;
                        pkt.thread_id = thread_to_try;
                        
                        raw_tlp.write(pkt);
                        raw_valid.write(true);
                        
                        // Consume one credit for this thread
                        credit_counter[thread_to_try]--;
                        
                        // Print packet sent and remaining credits (now removed print)
                        
                        // Increment packet sequence
                        packet_seq++;
                        
                        // Update round-robin pointer
                        current_thread = (thread_to_try + 1) % 3;
                        
                        // We've sent a packet, exit the loop
                        break;
                    }
                }
            }
            
            // Wait for next clock edge or credit event
            // This is the only wait in the loop
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

    sc_out<sc_uint<3>>  credit_out;  // 3-bit credit bus for 3 threads

    // Standard C++ queues for each thread
    std::queue<RawTLP>     fifo0;
    std::queue<RawTLP>     fifo1;
    std::queue<RawTLP>     fifo2;
    
    // Internal state
    static const unsigned int FIFO_CAPACITY = 8;
    sc_event fifo_state_changed[3]; // Events to signal FIFO state changes for each thread

    // Thread for receiving TLPs
    void receiver_thread() {
        while (true) {
            // Wait for the next clock edge
            wait(clk.posedge_event());
            
            if (!reset_n.read()) {
                // Reset state - clear all FIFOs
                while (!fifo0.empty()) fifo0.pop();
                while (!fifo1.empty()) fifo1.pop();
                while (!fifo2.empty()) fifo2.pop();
                
                for (int i = 0; i < 3; i++) {
                    fifo_state_changed[i].notify();
                }
            } else {
                // Check for incoming packet
                if (raw_valid.read()) {
                    RawTLP pkt = raw_tlp.read();
                    unsigned int thread_id = pkt.thread_id.to_uint();
                    
                    // Ensure valid thread_id
                    if (thread_id < 3) {
                        bool pushed = false;
                        
                        // Select the correct FIFO based on thread_id
                        if (thread_id == 0 && fifo0.size() < FIFO_CAPACITY) {
                            fifo0.push(pkt);
                            pushed = true;
                        } else if (thread_id == 1 && fifo1.size() < FIFO_CAPACITY) {
                            fifo1.push(pkt);
                            pushed = true;
                        } else if (thread_id == 2 && fifo2.size() < FIFO_CAPACITY) {
                            fifo2.push(pkt);
                            pushed = true;
                        }
                        
                        if (pushed) {
                            // Debug print for packet reception (now removed print)
                            
                            // Notify FIFO state changed
                            fifo_state_changed[thread_id].notify();
                        }
                    }
                }
            }
        }
    }
    
    // Thread for generating credit pulses
    void credit_generator_thread() {
        // Initialize
        credit_out.write(0);
        
        // Track total credits issued per thread
        unsigned int credits_issued[3] = {0, 0, 0};
        
        while (true) {
            // Wait for the next clock edge or any FIFO state change
            sc_event_or_list fifo_events;
            for (int i = 0; i < 3; i++) {
                fifo_events |= fifo_state_changed[i];
            }
            
            wait(clk.posedge_event() | fifo_events);
            
            if (!reset_n.read()) {
                // Reset state
                credit_out.write(0);
                for (int i = 0; i < 3; i++) {
                    credits_issued[i] = 0;
                }
                // Reset print (now removed)
            } else {
                // Default - no credit pulse on any bit
                credit_out.write(0);
                
                // Check each FIFO and generate credit if there is space
                sc_uint<3> credits = 0;
                
                // Get current FIFO usage
                unsigned int fifo_used[3];
                fifo_used[0] = fifo0.size();
                fifo_used[1] = fifo1.size();
                fifo_used[2] = fifo2.size();
                
                for (int i = 0; i < 3; i++) {
                    // Calculate available space in the FIFO
                    unsigned int available_space = FIFO_CAPACITY - fifo_used[i];
                    
                    // Only issue credits if:
                    // 1. There is space in the FIFO
                    // 2. Total credits issued won't exceed FIFO capacity
                    // 3. Total outstanding credits per thread is capped
                    if (available_space > 0 && credits_issued[i] < FIFO_CAPACITY) {
                        // Ensure we're not issuing more credits than what fits in FIFO
                        if (credits_issued[i] - fifo_used[i] < available_space) {
                            // FIFO i has space, set its credit bit
                            credits[i] = 1;
                            credits_issued[i]++;
                        }
                    }
                }
                
                // If any credits are available, send a pulse - ensure one-cycle pulse
                if (credits != 0) {
                    // Generate credit pulse - exactly one clock cycle
                    credit_out.write(credits);  // Send pulse
                    wait(clk.posedge_event());  // Wait one cycle
                    credit_out.write(0);        // End pulse
                }
            }
        }
    }

    // Constructor 
    SC_CTOR(iEP) {
        // Register the threads
        SC_THREAD(receiver_thread);
        sensitive << clk.pos();
        
        SC_THREAD(credit_generator_thread);
        sensitive << clk.pos() << fifo_state_changed[0] << fifo_state_changed[1] << fifo_state_changed[2];
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

