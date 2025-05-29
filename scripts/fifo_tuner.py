import re, subprocess, shutil, os, sys, itertools, textwrap, csv

SRC_FILE = 'src/main.cpp'
LOG_FILE = 'auto_run.log'
ANALYZE = 'scripts/analyze_logs.py'

TX_DEPTHS = [2, 4, 8, 16, 24, 32, 64, 128, 256, 512, 1024]
RX_DEPTHS = [2, 4, 8, 16, 24]
TARGET_RATIO = 0.9  # ready throughput should be this fraction of credit throughput

# Regex matches the DEPTH line; we capture the number so we can replace only once per module
DEPTH_REGEX = re.compile(r'static const unsigned DEPTH = (\d+);')


def set_depths(tx_depth: int, rx_depth: int):
    """Replace first DEPTH constant (TX) and second DEPTH constant (RX)"""
    with open(SRC_FILE, 'r') as f:
        lines = f.readlines()

    replace_count = 0
    new_lines = []
    for line in lines:
        m = DEPTH_REGEX.search(line)
        if m and replace_count < 2:
            new_val = tx_depth if replace_count == 0 else rx_depth
            new_line = DEPTH_REGEX.sub(f'static const unsigned DEPTH = {new_val};', line)
            new_lines.append(new_line)
            replace_count += 1
        else:
            new_lines.append(line)

    if replace_count != 2:
        raise RuntimeError('DEPTH constants (TX, RX) not found as expected')

    with open(SRC_FILE, 'w') as f:
        f.writelines(new_lines)


def build():
    subprocess.check_call(['make'])


def run_sim():
    with open(LOG_FILE, 'w') as f:
        subprocess.check_call(['./build/sim'], stdout=f, stderr=subprocess.STDOUT)


def analyze():
    """Return (credit_Mpps, ready_Mpps) from last log."""
    out = subprocess.check_output(['python3', ANALYZE, LOG_FILE], text=True)
    credit_tp = ready_tp = None
    current = None
    for line in out.splitlines():
        line = line.strip()
        if line.startswith('Credit path:'):
            current = 'credit'
            continue
        if line.startswith('Ready path:'):
            current = 'ready'
            continue
        if 'Throughput' in line and current:
            parts = line.split()
            try:
                mpps = float(parts[-2])
            except ValueError:
                continue
            if current == 'credit':
                credit_tp = mpps
            elif current == 'ready':
                ready_tp = mpps
            current = None
    return credit_tp, ready_tp


def sweep():
    results = []
    print('\nTX_DEPTH RX_DEPTH  Ready(Mpps)  Ratio_to_Credit')
    print('-'*45)
    for tx, rx in itertools.product(TX_DEPTHS, RX_DEPTHS):
        set_depths(tx, rx)
        build()
        run_sim()
        credit, ready = analyze()
        if ready is None:
            print(f'{tx:8d} {rx:8d}   ---           N/A   PARSE_ERR')
            continue
        ratio = ready / credit if credit else 0.0
        status = 'OK' if ratio >= TARGET_RATIO else 'FAIL'
        print(f'{tx:8d} {rx:8d}   {ready:8.2f}      {ratio:5.2f} {status}')
        results.append((tx, rx, credit, ready, ratio, status))

    # write CSV report
    report_path = 'fifo_sweep_report.csv'
    with open(report_path, 'w', newline='') as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(['tx_depth', 'rx_depth', 'credit_mpps', 'ready_mpps', 'ratio', 'status'])
        for row in results:
            writer.writerow(row)

    print(f"\nSweep complete. CSV report written to {report_path}")


def main():
    sweep()

if __name__ == '__main__':
    main() 