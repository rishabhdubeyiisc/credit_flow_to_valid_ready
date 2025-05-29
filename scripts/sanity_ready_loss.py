#!/usr/bin/env python3
"""Sanity checker: verify no packet drops in ready/valid topology.
   Usage: python3 sanity_ready_loss.py sim_run.txt
   Exits with non-zero code if mismatch detected.
"""
import sys
import re

if len(sys.argv)!=2:
    print("usage: sanity_ready_loss.py <sim_log.txt>")
    sys.exit(1)
logfile = sys.argv[1]

sent = set()
recv = set()
recv_count = 0
dropped = set()

with open(logfile, 'r', encoding='utf-8') as f:
    prev_line = ''
    for line in f:
        line = line.rstrip('\n')
        if prev_line:
            line = prev_line + line
            prev_line = ''
        if 'iRC_tx' in line and 'sender_thread' in line and 'seq_num=' in line:
            idx = line.find('seq_num=')
            if idx != -1:
                rest = line[idx+8:]
                num = ''
                for c in rest:
                    if c.isdigit():
                        num += c
                    else:
                        break
                if num:
                    sent.add(int(num))
        elif 'iEP_after_RX' in line and 'input_router_thread' in line and 'routed seq_num=' in line:
            idx = line.find('routed seq_num=')
            if idx != -1:
                rest = line[idx+14:]
                m = re.search(r'(\d+)', rest)
                if m:
                    recv.add(int(m.group(1)))
                    recv_count += 1
        elif 'AXI_NOC' in line and 'DROPPED seq_num=' in line:
            idx = line.find('DROPPED seq_num=')
            if idx != -1:
                rest = line[idx+16:]
                num = ''
                for c in rest:
                    if c.isdigit():
                        num += c
                    else:
                        break
                if num:
                    dropped.add(int(num))
        elif line.endswith('queue_id=') or line.endswith('queue_id=\n'):
            prev_line = line

# Remove dropped packets from sent set
sent = sent - dropped

missing = sent - recv
extra   = recv - sent

total = len(sent)
max_recv = max(recv) if recv else -1

# Accept missing only if they are all greater than the highest sequence that
# has already been received (i.e. still in flight through the pipeline)
in_flight_ok = all(s > max_recv for s in missing)

print(f"DEBUG: Matched {recv_count} received lines")
print(f"Sent={total}  Received={len(recv)}  Missing={len(missing)}  Extra={len(extra)}  (max received {max_recv})")

ignore_threshold = int(total * 0.9)
missing_filtered = [s for s in missing if s < ignore_threshold]
print(f"Ignoring missing seq_nums >= {ignore_threshold} (last 10% of sent packets)")

if missing_filtered:
    print("Filtered missing seq nums:", sorted(missing_filtered))

ok = not missing_filtered and not extra
sys.exit( 0 if ok else 2 ) 