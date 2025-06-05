#ifndef CONFIG_H
#define CONFIG_H

// -----------------------------------------------------------------------------
// Central build-time configuration (all sizes / latencies in one place)
// -----------------------------------------------------------------------------

constexpr unsigned TX_FIFO_DEPTH   = 1024;   // entries (24 packets @ 64-bit)
constexpr unsigned RX_FIFO_DEPTH   = 2;   // entries
constexpr unsigned THREAD_Q_DEPTH = 8;   // per-thread depth inside iEP/FrontEnd
const unsigned CREDIT_SENSE_WINDOW = 8; // can be tuned – equal to Threaded FIFO depth
constexpr unsigned sim_time_in_us = 10000;  // total simulation duration

constexpr unsigned DATA_NOC_LATENCY = 100;
constexpr unsigned DATA_NOC_STALL_PCT = 5;

constexpr unsigned CREDIT_NOC_LATENCY = 100;
constexpr unsigned CREDIT_NOC_STALL_PCT = 5;

constexpr unsigned NOC_PATTERN_LEN = 100; // resolution (cycles)
static_assert(DATA_NOC_STALL_PCT < 100, "data NOC stall percentage must be <100");
static_assert(CREDIT_NOC_STALL_PCT < 100, "credit NOC stall percentage must be <100");

// Configuration structure for global settings
struct GlobalConfig {
    static bool enable_popping;
};

#endif // CONFIG_H 