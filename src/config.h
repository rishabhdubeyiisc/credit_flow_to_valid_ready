#ifndef CONFIG_H
#define CONFIG_H

// -----------------------------------------------------------------------------
// Central build-time configuration (all sizes / latencies in one place)
// -----------------------------------------------------------------------------

constexpr unsigned TX_FIFO_DEPTH   = 16;   // entries (24 packets @ 64-bit)
constexpr unsigned RX_FIFO_DEPTH   = 2;   // entries
constexpr unsigned THREAD_Q_DEPTH = 8;   // per-thread depth inside iEP/FrontEnd
const unsigned GLOBAL_SENSE_WINDOW = 8; // can be tuned – equal to Threaded FIFO depth
constexpr unsigned sim_time_in_us = 400;  // total simulation duration

constexpr unsigned NOC_STATIC_LATENCY_ONE_WAY = ((2*65) + 20) ;  //((2*65) + 20)// fixed AXI cycles through NoC //taking from NIC Fabric thus 2 times as C2C + GPU fabrics + Lets take C2C and SMN bridges to add 10 Cycles // this in one way latency only
constexpr unsigned NOC_STALL_PCT  = 15; // percentage (0-99) of cycles ready is LOW
constexpr unsigned NOC_PATTERN_LEN = 100; // resolution (cycles)
static_assert(NOC_STALL_PCT < 100, "stall percentage must be <100");

// Configuration structure for global settings
struct GlobalConfig {
    static bool enable_popping;
};

#endif // CONFIG_H 