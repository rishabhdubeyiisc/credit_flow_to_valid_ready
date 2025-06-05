#include "modules.h"

void Threaded_Queue::main_thread()
{
    wait(SC_ZERO_TIME); // Initial wait for proper synchronization

    // Initialize
    credits = 0;
    credit_pending = false;
    credit_out.write(false);

    while (true)
    {
        wait(clk.posedge_event());
        // Allow router to update tlp_signals this cycle
        wait(SC_ZERO_TIME);

        if (!reset_n.read())
        {
            // Reset state
            RawTLP dummy;
            while (fifo.nb_read(dummy))
            {
            } // Clear the FIFO
            credits = 0;
            credit_pending = false;
            credit_out.write(false);
            continue;
        }

        // Handle credit updates first
        if (credit_pending)
        {
            credit_out.write(false);
            credit_pending = false;
        }

        // Enqueue packet if valid and space available
        if (valid_in.read() && fifo.num_free() > 0)
        {
            RawTLP pkt = raw_tlp_in.read();
            fifo.write(pkt);
            std::cout << sc_time_stamp() << " [" << name() << "] " << __FUNCTION__
                      << " seq_num=" << pkt.seq_num << " thread_id=" << pkt.thread_id
                      << " (FIFO occ=" << fifo.num_available() << ")" << std::endl;
        }

        // Issue credit if space available and credits not maxed out
        if (credits < capacity && fifo.num_free() > 0)
        {
            credits++;
            credit_out.write(true);
            credit_pending = true;
            std::cout << sc_time_stamp() << " [" << name() << "] " << __FUNCTION__
                      << " Issuing credit - Current=" << credits << std::endl;
        }
    }
}

// Method to check if data is available
bool Threaded_Queue::has_data() const
{
    return fifo.num_available() > 0;
}

// Method to pop data from FIFO
bool Threaded_Queue::pop_data(RawTLP &pkt)
{
    if (fifo.nb_read(pkt))
    { // Use non-blocking read
        if (credits > 0)
        {
            credits--;
            std::cout << sc_time_stamp() << " [" << name() << "] " << __FUNCTION__
                      << " seq_num=" << pkt.seq_num << " thread_id=" << pkt.thread_id
                      << " - Credits=" << credits << std::endl;
        }
        return true;
    }
    return false;
}

bool ThreadedFrontEnd::has_data(int idx) const
{
    return queues[idx]->has_data();
}

bool ThreadedFrontEnd::pop_data(int idx, RawTLP &pkt)
{
    return queues[idx]->pop_data(pkt);
}

// Router thread: demux ingress packet by thread_id -> per-queue signals
void ThreadedFrontEnd::input_router_thread()
{
    while (true)
    {
        wait(clk.posedge_event());

        if (!reset_n.read())
        {
            // de-assert all valids when in reset
            for (int i = 0; i < 3; ++i)
                valid_signals[i].write(false);
            continue;
        }

        wait(SC_ZERO_TIME); // 1-delta to allow producer observations

        for (int i = 0; i < 3; ++i)
            valid_signals[i].write(false);

        if (ingress_valid.read())
        {
            RawTLP pkt = ingress_tlp.read();
            unsigned tid = pkt.thread_id.to_uint();
            if (tid >= 1 && tid <= 3)
            {
                tlp_signals[tid - 1].write(pkt);
                valid_signals[tid - 1].write(true);
                std::cout << sc_time_stamp() << " [" << name() << "] " << __FUNCTION__
                          << " routed seq_num=" << pkt.seq_num
                          << " thread_id=" << pkt.thread_id
                          << " queue_id=" << (tid - 1) << std::endl;
            }
        }
    }
}

// Combiner: OR-reduce per-queue credit pulses into a 3-bit bus
void ThreadedFrontEnd::credit_combine_thread()
{
    while (true)
    {
        wait(clk.posedge_event());
        wait(SC_ZERO_TIME);
        sc_uint<3> combined = 0;
        for (int i = 0; i < 3; ++i)
            combined[i] = credit_signals[i].read();
        credit_out.write(combined);
    }
}

void SimpleTxFIFO::main_thread()
{
    wait(SC_ZERO_TIME);
    bool holding = false;
    RawTLP held_pkt;
    while (true)
    {
        wait(clk.posedge_event());

        // enqueue from iRC_tx
        if (ingress_valid.read() && fifo.num_free() > 0)
        {
            fifo.write(ingress_tlp.read());
        }

        // track occupancy
        unsigned cur_occ = fifo.num_available() + (holding ? 1 : 0);
        if (cur_occ > max_occ)
        {
            max_occ = cur_occ;
            std::cout << sc_time_stamp()
                      << " [TX_FIFO] depth=" << max_occ << std::endl;
        }

        // fetch new packet when current TLP fully sent
        if (!holding && fifo.nb_read(held_pkt))
        {
            holding = true;
            std::cout << sc_time_stamp() << " [" << name() << "] " << __FUNCTION__
                      << " Ready to dequeue seq_num=" << held_pkt.seq_num
                      << " thread_id=" << held_pkt.thread_id << std::endl;
        }

        // drive outputs
        if (holding)
        {
            AxiWord w = tlp_to_axi(held_pkt);
            egress_axi.write(w);
            egress_valid.write(true);

            if (egress_ready.read())
            {
                std::cout << sc_time_stamp() << " [" << name() << "] " << __FUNCTION__
                          << " Handshake: sent seq_num=" << held_pkt.seq_num
                          << " thread_id=" << held_pkt.thread_id << std::endl;
                holding = false; // consumer accepted in this cycle
            }
        }
        else
        {
            egress_valid.write(false);
        }
    }
}

// -----------------------------------------------------------------------------
// SimpleRxFIFO: single FIFO buffer; accepts valid/ready, outputs raw_valid/raw_tlp.
// -----------------------------------------------------------------------------

void SimpleRxFIFO::main_thread()
{
    wait(SC_ZERO_TIME);
    while (true)
    {
        wait(clk.posedge_event());

        ready_out.write(fifo.num_free() > 0);

        unsigned int occ = fifo.num_available();
        if (occ > max_occ)
        {
            max_occ = occ;
            std::cout << sc_time_stamp() << " [RX_FIFO] depth=" << max_occ << std::endl;
        }

        if (valid_in.read() && ready_out.read())
        {
            AxiWord aw = axi_in.read();
            RawTLP p = axi_to_tlp(aw);
            fifo.write(p);
            std::cout << sc_time_stamp() << " [" << name() << "] " << __FUNCTION__
                      << " Enqueue seq_num=" << p.seq_num << " thread_id=" << p.thread_id << std::endl;
        }

        RawTLP pkt;
        if (fifo.nb_read(pkt))
        {
            tlp_out.write(pkt);
            valid_out.write(true);
        }
        else
        {
            valid_out.write(false);
        }
    }
}

void CreditTx::main_thread()
{
    wait(SC_ZERO_TIME);
    while (true)
    {
        wait(clk.posedge_event());

        if (!reset_n.read())
        {
            for (int i = 0; i < 3; ++i)
                accum[i] = 0;
            ctr = 0;
            sending = false;
            valid_out.write(false);
            continue;
        }

        // Default deassert
        if (!sending)
            valid_out.write(false);

        // count incoming credit pulses every cycle
        sc_uint<3> in = credit_in.read();
        for (int i = 0; i < 3; ++i)
            if (in[i])
                accum[i]++;

        // When not currently sending, check window expiry
        if (!sending)
        {
            if (++ctr == window_size)
            {
                ctr = 0;
                // Build packet even if some counts are zero – optional
                pending = credits_to_axi(accum[0], accum[1], accum[2]);
                for (int i = 0; i < 3; ++i)
                    accum[i] = 0;
                sending = true;
                valid_out.write(true);
                axi_out.write(pending);
            }
        }
        else
        { // currently asserting valid
            if (ready_in.read())
            {
                sending = false; // beat accepted
                valid_out.write(false);
            }
            else
            {
                // keep driving same word
                valid_out.write(true);
                axi_out.write(pending);
            }
        }
    }
}

void CreditRx::main_thread()
{
    wait(SC_ZERO_TIME);
    while (true)
    {
        wait(clk.posedge_event());

        if (!reset_n.read())
        {
            for (int i = 0; i < 3; ++i)
                emit_cnt[i] = 0;
            ready_out.write(true);
            credit_out.write(0);
            continue;
        }

        // Default outputs
        credit_out.write(0);

        bool empty = (emit_cnt[0] == 0 && emit_cnt[1] == 0 && emit_cnt[2] == 0);
        ready_out.write(empty); // accept new packet only when previous drained

        // Emit phase
        if (!empty)
        {
            sc_uint<3> pulse = 0;
            for (int i = 0; i < 3; ++i)
            {
                if (emit_cnt[i] != 0)
                {
                    pulse[i] = 1;
                    emit_cnt[i]--;
                }
            }
            credit_out.write(pulse);
        }

        // Acceptance of new packet
        if (valid_in.read() && ready_out.read())
        {
            sc_uint<16> cnt0, cnt1, cnt2;
            axi_to_credits(axi_in.read(), cnt0, cnt1, cnt2);
            emit_cnt[0] = cnt0;
            emit_cnt[1] = cnt1;
            emit_cnt[2] = cnt2;
        }
    }
}

// Thread for monitoring credit pulses
void iRC::credit_monitor_thread()
{
    // Initialize credit counters
    for (int i = 0; i < 3; i++)
    {
        credit_counter[i] = 0;
    }

    while (true)
    {
        wait(clk.posedge_event());

        if (!reset_n.read())
        {
            // Reset state
            for (int i = 0; i < 3; i++)
            {
                credit_counter[i] = 0;
            }
        }
        else
        {
            // Sample credit pulse on any bit
            sc_uint<3> credits = credit_in.read();
            bool credit_received = false;

            for (int i = 0; i < 3; i++)
            {
                if (credits[i])
                {
                    // Credit pulse received for thread i+1 (since threads are 1-3)
                    credit_counter[i]++;
                    credit_received = true;
                }
            }

            if (credit_received)
            {
                credit_event.notify();
            }
        }
    }
}

// Thread for sending TLPs
void iRC::sender_thread()
{
    // Initialize
    packet_seq = 1;
    raw_valid.write(false);

    static int current_thread = 1; // Persistent round-robin pointer

    while (true)
    {
        wait(clk.posedge_event()); // Main clock-driven loop

        if (!reset_n.read())
        {
            // Reset state
            packet_seq = 1;
            raw_valid.write(false);
            continue;
        }

        // By default, deassert valid every cycle
        raw_valid.write(false);

        // Try to send a packet if we have credits for any thread
        for (int i = 0; i < 3; i++)
        {
            int thread_to_try = ((current_thread - 1 + i) % 3) + 1; // Map 0-2 to 1-3

            if (credit_counter[thread_to_try - 1] > 0)
            { // Adjust index for credit_counter
                // Create and send a packet for this thread
                RawTLP pkt;
                pkt.seq_num = packet_seq;
                pkt.thread_id = thread_to_try;

                raw_tlp.write(pkt);
                raw_valid.write(true); // Assert valid for this cycle only
                std::cout << sc_time_stamp() << " [" << name() << "] " << __FUNCTION__
                          << " seq_num=" << pkt.seq_num << " thread_id=" << pkt.thread_id << std::endl;

                // Consume one credit for this thread
                credit_counter[thread_to_try - 1]--; // Adjust index for credit_counter

                // Increment packet sequence
                packet_seq++;

                // Update round-robin pointer to the next thread
                current_thread = thread_to_try + 1;
                if (current_thread > 3)
                    current_thread = 1;

                break; // Only send one packet per cycle
            }
        }
    }
}

// Method to process popped data
void iEP::process_popped_data(const RawTLP &pkt, int queue_id)
{
    std::cout << sc_time_stamp() << " [" << name() << "] " << __FUNCTION__
              << " queue_id=" << queue_id << " seq_num=" << pkt.seq_num
              << " thread_id=" << pkt.thread_id << std::endl;
}

void iEP::popper_thread()
{
    unsigned int pop_counter = 0; // Counter for deterministic popping

    while (true)
    {
        wait(clk.posedge_event());

        if (!reset_n.read())
        {
            pop_counter = 0;
            continue;
        }

        // Pop from queues when enabled
        if (GlobalConfig::enable_popping)
        {
            std::cout << sc_time_stamp() << " [iEP popper] counter=" << pop_counter << std::endl;
            if (pop_counter == 3)
            { // Pop on every 4th cycle
                // Try to pop from each queue
                for (int i = 0; i < 3; i++)
                {
                    RawTLP pkt;
                    if (threaded_queues->pop_data(i, pkt))
                    {
                        process_popped_data(pkt, i);
                    }
                }
            }
            pop_counter = (pop_counter + 1) % 4; // Cycle counter 0-3
        }
    }
}

void AxiNoC::main_thread()
{
    wait(SC_ZERO_TIME);
    while (true)
    {
        wait(clk.posedge_event());

        // Predict stall condition for next cycle
        const unsigned next_pattern_ctr = (pattern_ctr + 1) % NOC_PATTERN_LEN;
        const unsigned stall_cycles = (NOC_PATTERN_LEN * NOC_STALL_PCT) / 100;
        bool next_stall_active = (next_pattern_ctr < stall_cycles);

        // Only assert ready if we won't stall next cycle
        bool ready_ok = !pipe_valid[0] && !next_stall_active;
        ready_out.write(ready_ok);

        if (valid_in.read() && ready_ok)
        {
            pipe[0] = axi_in.read();
            pipe_valid[0] = true;
            if (is_main_noc)
                std::cout << sc_time_stamp() << " [" << name() << "] " << __FUNCTION__
                          << " ingress seq_num=" << axi_to_tlp(pipe[0]).seq_num << std::endl;
        }

        // Update pattern counter for next cycle
        pattern_ctr = next_pattern_ctr;

        // Drive output when last stage valid
        if (pipe_valid[PIPE_LAT - 1])
        {
            valid_out.write(true);
            axi_out.write(pipe[PIPE_LAT - 1]);
            if (is_main_noc)
                std::cout << sc_time_stamp() << " [" << name() << "] " << __FUNCTION__
                          << " EGRESS seq_num=" << axi_to_tlp(pipe[PIPE_LAT - 1]).seq_num << std::endl;
            if (ready_in.read())
            {
                pipe_valid[PIPE_LAT - 1] = false;
                if (is_main_noc)
                    std::cout << sc_time_stamp() << " [" << name() << "] " << __FUNCTION__
                              << " ACCEPTED seq_num=" << axi_to_tlp(pipe[PIPE_LAT - 1]).seq_num << std::endl;
            }
        }
        else
        {
            valid_out.write(false);
        }

        // shift pipeline each clock
        for (int i = PIPE_LAT - 1; i > 0; --i)
        {
            if (!pipe_valid[i] && pipe_valid[i - 1])
            {
                pipe[i] = pipe[i - 1];
                pipe_valid[i] = true;
                pipe_valid[i - 1] = false;
            }
        }

        if (valid_in.read() && !ready_ok && is_main_noc)
        {
            std::cout << sc_time_stamp() << " [" << name() << "] " << __FUNCTION__
                      << " DROPPED seq_num=" << axi_to_tlp(axi_in.read()).seq_num << " (backpressure)" << std::endl;
        }
    }
}

void CreditDutyMon::sample()
{
    total++;
    if (bus_direct.read() != 0)
        hi_direct++;
    if (bus_hybrid.read() != 0)
        hi_hybrid++;
}

void CreditDutyMon::report()
{
    std::cout << "\n---- Credit bus duty cycle ----\n";
    if (total == 0)
    {
        std::cout << "No samples taken!\n";
        return;
    }
    auto pct = [&](sc_uint<64> hi)
    { return 100.0 * static_cast<double>(hi) / static_cast<double>(total); };
    std::cout << "Direct bus : " << pct(hi_direct) << " %\n";
    std::cout << "Hybrid bus : " << pct(hi_hybrid) << " %\n";
}
