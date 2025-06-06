import re, subprocess, shutil, os, sys, itertools, textwrap, csv

SRC_FILE = 'src/config.h'
LOG_FILE = 'auto_run.log'
ANALYZE = 'scripts/analyze_logs.py'
SANITY = 'scripts/sanity_ready_loss.py'

TX_DEPTHS = [1024, 256, 32, 16, 8, 4, 2, 1]
RX_DEPTHS = [4,2,1]
TARGET_RATIO = 0.8  # ready throughput should be this fraction of credit throughput

# Regexes to locate the TX and RX depth constexpr lines in main.cpp
TX_REGEX = re.compile(r'constexpr unsigned TX_FIFO_DEPTH\s*=\s*(\d+);')
RX_REGEX = re.compile(r'constexpr unsigned RX_FIFO_DEPTH\s*=\s*(\d+);')


def set_depths(tx_depth: int, rx_depth: int):
    """Replace first DEPTH constant (TX) and second DEPTH constant (RX)"""
    print(f"\nSetting depths: TX={tx_depth}, RX={rx_depth}")
    with open(SRC_FILE, 'r') as f:
        lines = f.readlines()

    tx_done = rx_done = False
    new_lines = []
    for line in lines:
        if not tx_done and TX_REGEX.search(line):
            new_lines.append(TX_REGEX.sub(f'constexpr unsigned TX_FIFO_DEPTH   = {tx_depth};', line))
            tx_done = True
            print(f"Found and updated TX_FIFO_DEPTH")
            continue
        if not rx_done and RX_REGEX.search(line):
            new_lines.append(RX_REGEX.sub(f'constexpr unsigned RX_FIFO_DEPTH   = {rx_depth};', line))
            rx_done = True
            print(f"Found and updated RX_FIFO_DEPTH")
            continue
        new_lines.append(line)

    if not (tx_done and rx_done):
        raise RuntimeError('TX_FIFO_DEPTH or RX_FIFO_DEPTH constant not found')

    with open(SRC_FILE, 'w') as f:
        f.writelines(new_lines)
    print("Successfully updated config.h")


def build():
    print("\nBuilding project...")
    subprocess.check_call(['make', 'clean'])
    subprocess.check_call(['make'])
    print("Build complete")


def run_sim():
    print("\nRunning simulation...")
    with open(LOG_FILE, 'w') as f:
        subprocess.check_call(['./build/sim'], stdout=f, stderr=subprocess.STDOUT)
    print("Simulation complete")
    
    # Print first few lines of log for debugging
    print("\nFirst few lines of simulation log:")
    with open(LOG_FILE, 'r') as f:
        print(''.join(f.readlines()[:10]))


def analyze():
    """Parse analysis output into a dict of metrics."""
    print("\nAnalyzing results...")
    metrics = {}
    try:
        txt = subprocess.check_output(['python3', ANALYZE, LOG_FILE], text=True)
        print("\nAnalysis output:")
        print(txt)
        
        cur = None
        for line in txt.splitlines():
            line = line.strip()
            if line.startswith('Credit path:'):
                cur = 'credit'
                continue
            if line.startswith('Ready path:'):
                cur = 'ready'
                continue
            if line.startswith('Packets received') and cur:
                metrics[f'{cur}_pkts'] = int(line.split(':')[1])
                continue
            if line.startswith('Avg latency') and cur:
                metrics[f'{cur}_lat'] = float(line.split(':')[1].split()[0])
                continue
            if line.startswith('Throughput') and cur:
                metrics[f'{cur}_mpps'] = float(line.split()[-2])
                cur = None
                continue
            if line.startswith('Max TX FIFO'):
                metrics['max_tx'] = int(line.split(':')[1])
                continue
            if line.startswith('Max RX FIFO'):
                metrics['max_rx'] = int(line.split(':')[1])
                continue
            if line.startswith('Direct bus'):
                metrics['duty_direct'] = float(line.split(':')[1].strip().rstrip('%'))
                continue
            if line.startswith('Hybrid bus'):
                metrics['duty_hybrid'] = float(line.split(':')[1].strip().rstrip('%'))
                continue
    except subprocess.CalledProcessError as e:
        print(f"Error running analysis script: {e}")
        print("Analysis script output:")
        print(e.output if hasattr(e, 'output') else "No output available")
    return metrics


def sanity_ok():
    print("\nRunning sanity check...")
    rc = subprocess.call(['python3', SANITY, LOG_FILE])
    print(f"Sanity check {'passed' if rc == 0 else 'failed'}")
    return rc == 0


def sweep():
    results = []
    print('\nTX_DEPTH RX_DEPTH  Ready(Mpps)  Ratio_to_Credit')
    print('-'*45)
    for tx, rx in itertools.product(TX_DEPTHS, RX_DEPTHS):
        set_depths(tx, rx)
        build()
        run_sim()

        sane = sanity_ok()

        m = analyze() if sane else {}

        if not sane:
            print(f"{tx:8d} {rx:8d}   ---           N/A   DROPPED")
            results.append((tx, rx, None, None, None, None, 'DROP'))
            continue

        if 'ready_mpps' not in m:
            print(f'{tx:8d} {rx:8d}   ---           N/A   PARSE_ERR')
            continue
        ratio = m['ready_mpps'] / m['credit_mpps'] if m['credit_mpps'] else 0.0
        status = 'OK' if ratio >= TARGET_RATIO else 'FAIL'
        print(f"{tx:8d} {rx:8d}   {m['ready_mpps']:8.2f}      {ratio:5.2f} {status}")
        results.append((tx, rx,
                        m.get('credit_mpps'), m.get('credit_lat'), m.get('credit_pkts'),
                        m.get('ready_mpps'),  m.get('ready_lat'),  m.get('ready_pkts'),
                        ratio,
                        m.get('max_tx'), m.get('max_rx'),
                        m.get('duty_direct'), m.get('duty_hybrid'),
                        status))

    # write CSV report
    report_path = 'fifo_sweep_report.csv'
    print(f"\nWriting results to {report_path}")
    with open(report_path, 'w', newline='') as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(['tx_depth','rx_depth',
                         'credit_mpps','credit_lat_ns','credit_pkts',
                         'ready_mpps','ready_lat_ns','ready_pkts',
                         'ratio',
                         'max_tx','max_rx',
                         'duty_direct_%','duty_hybrid_%',
                         'status'])
        for row in results:
            writer.writerow(row)

    print(f"\nSweep complete. CSV report written to {report_path}")


def main():
    sweep()

if __name__ == '__main__':
    main() 