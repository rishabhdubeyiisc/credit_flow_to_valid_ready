import re
import sys
from collections import defaultdict

# Usage: python analyze_logs.py <sim_log.txt>
# Pass '-' to read from stdin

send_pattern = re.compile(r"(?P<time>\d+)\s+ns \[iRC(?P<tx>_tx)?\] Send seq=(?P<seq>\d+)")
# pop pattern captures queue name so we can identify topology via prefix
pop_pattern = re.compile(r"(?P<time>\d+)\s+ns \[(?P<queue>[^\]]+)\] Popping data: seq_num=(?P<seq>\d+)")
# track FIFO occupancy prints
depth_pattern = re.compile(r"\[(?P<which>[TR]X)_FIFO\] depth=(?P<d>\d+)")

if len(sys.argv) != 2:
    print("Usage: python analyze_logs.py <sim_log.txt>  (use '-' for stdin)")
    sys.exit(1)

source = sys.stdin if sys.argv[1] == '-' else open(sys.argv[1], 'r', encoding='utf-8')

send_ts = {
    'credit': {},  # seq -> time
    'ready': {},
}
latencies = defaultdict(list)  # topology -> list of ns
first_time = None
last_time = 0
# track FIFO occupancy prints
max_depth = {'TX': 0, 'RX': 0}

for line in source:
    line = line.strip()
    m = send_pattern.match(line)
    if m:
        t = int(m.group('time'))
        seq = int(m.group('seq'))
        topo = 'ready' if m.group('tx') else 'credit'
        send_ts[topo][seq] = t
        if first_time is None or t < first_time:
            first_time = t
        last_time = max(last_time, t)
        continue
    mp = pop_pattern.match(line)
    if mp:
        t = int(mp.group('time'))
        seq = int(mp.group('seq'))
        qname = mp.group('queue')
        if qname.startswith('iEP.iEP_front'):
            topo = 'credit'
        elif qname.startswith('iEP_after_RX'):
            topo = 'ready'
        else:
            # fallback based on substring
            topo = 'ready' if 'after_RX' in qname else 'credit'
        st = send_ts[topo].pop(seq, None)
        if st is not None:
            latencies[topo].append(t - st)
        if first_time is None or t < first_time:
            first_time = t
        last_time = max(last_time, t)
        continue

    md = depth_pattern.search(line)
    if md:
        which = md.group('which')  # 'TX' or 'RX'
        depth = int(md.group('d'))
        if depth > max_depth[which]:
            max_depth[which] = depth
        continue

if first_time is None:
    print("No events found – are you using the correct log?")
    sys.exit(1)

sim_duration_ns = last_time - first_time
print(f"Simulation duration: {sim_duration_ns/1e3:.3f} µs\n")

for topo in ('credit', 'ready'):
    n = len(latencies[topo])
    if n == 0:
        print(f"{topo.capitalize()} path: no packets received")
        continue
    avg_latency = sum(latencies[topo]) / n
    throughput_mpps = (n / (sim_duration_ns * 1e-9)) / 1e6  # packets/s -> Mpps
    bandwidth_MBps = (n * 8) / (sim_duration_ns * 1e-9) / 1e6  # 8 bytes per packet
    print(f"{topo.capitalize()} path:")
    print(f"  Packets received : {n}")
    print(f"  Avg latency      : {avg_latency:.1f} ns")
    print(f"  Throughput       : {throughput_mpps:.2f} Mpps")
    print(f"  Bandwidth        : {bandwidth_MBps:.2f} MB/s\n")

# FIFO occupancy summary
print(f"Max TX FIFO occupancy : {max_depth['TX']}")
print(f"Max RX FIFO occupancy : {max_depth['RX']}\n") 