#ifndef PAYLOADS_H
#define PAYLOADS_H

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
inline void sc_trace(sc_trace_file* tf, const RawTLP& tlp, const std::string& name) {
    sc_trace(tf, tlp.seq_num, name + ".seq_num");
    sc_trace(tf, tlp.thread_id, name + ".thread_id");
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
    // sc_trace(tf, w.tlast,  n+".tlast");
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
#endif // PAYLOADS_H