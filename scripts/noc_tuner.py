import re, subprocess, shutil, os, sys, itertools, textwrap, csv, time

SRC_FILE = 'src/config.h'
LOG_FILE = 'auto_run.log'
ANALYZE = 'scripts/analyze_logs.py'
SANITY = 'scripts/sanity_ready_loss.py'

STALL_PCTS = [25 , 15, 5, 1]
LATENCIES = [1, 20, 40, 60, 80]
TARGET_RATIO = 0.5  # RXTX throughput should be this fraction of Direct throughput
PACKET_SIZE_BYTES = 8  # 64-bit packets

# Regexes to locate the NOC parameter lines in config.h
DATA_NOC_LATENCY_REGEX = re.compile(r'constexpr unsigned DATA_NOC_LATENCY\s*=\s*(\d+);')
DATA_NOC_STALL_REGEX = re.compile(r'constexpr unsigned DATA_NOC_STALL_PCT\s*=\s*(\d+);')
CREDIT_NOC_LATENCY_REGEX = re.compile(r'constexpr unsigned CREDIT_NOC_LATENCY\s*=\s*(\d+);')
CREDIT_NOC_STALL_REGEX = re.compile(r'constexpr unsigned CREDIT_NOC_STALL_PCT\s*=\s*(\d+);')

def set_noc_params(latency: int, stall_pct: int):
    """Replace NOC latency and stall parameters in config.h"""
    print(f"\nSetting NOC parameters in {SRC_FILE}:")
    print(f"  DATA_NOC_LATENCY = {latency}")
    print(f"  DATA_NOC_STALL_PCT = {stall_pct}")
    print(f"  CREDIT_NOC_LATENCY = {latency}")
    print(f"  CREDIT_NOC_STALL_PCT = {stall_pct}")

    with open(SRC_FILE, 'r') as f:
        lines = f.readlines()

    new_lines = []
    for line in lines:
        if DATA_NOC_LATENCY_REGEX.search(line):
            new_lines.append(f'constexpr unsigned DATA_NOC_LATENCY = {latency};\n')
        elif DATA_NOC_STALL_REGEX.search(line):
            new_lines.append(f'constexpr unsigned DATA_NOC_STALL_PCT = {stall_pct};\n')
        elif CREDIT_NOC_LATENCY_REGEX.search(line):
            new_lines.append(f'constexpr unsigned CREDIT_NOC_LATENCY = {latency};\n')
        elif CREDIT_NOC_STALL_REGEX.search(line):
            new_lines.append(f'constexpr unsigned CREDIT_NOC_STALL_PCT = {stall_pct};\n')
        else:
            new_lines.append(line)

    with open(SRC_FILE, 'w') as f:
        f.writelines(new_lines)

    # Verify the changes were made
    with open(SRC_FILE, 'r') as f:
        content = f.read()
        if f'DATA_NOC_LATENCY = {latency}' not in content:
            raise RuntimeError(f"Failed to set DATA_NOC_LATENCY to {latency}")
        if f'DATA_NOC_STALL_PCT = {stall_pct}' not in content:
            raise RuntimeError(f"Failed to set DATA_NOC_STALL_PCT to {stall_pct}")
        if f'CREDIT_NOC_LATENCY = {latency}' not in content:
            raise RuntimeError(f"Failed to set CREDIT_NOC_LATENCY to {latency}")
        if f'CREDIT_NOC_STALL_PCT = {stall_pct}' not in content:
            raise RuntimeError(f"Failed to set CREDIT_NOC_STALL_PCT to {stall_pct}")
    print("Parameters verified in config file")

def clean_build():
    """Clean and rebuild the project"""
    print("\nCleaning and rebuilding project...")
    try:
        # First check if build directory exists
        if os.path.exists('build'):
            print("Removing build directory...")
            shutil.rmtree('build')
        
        # Run make clean
        print("Running make clean...")
        subprocess.check_call(['make', 'clean'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        
        # Run make
        print("Running make...")
        subprocess.check_call(['make'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        
        # Verify build/sim exists
        if not os.path.exists('build/sim'):
            raise RuntimeError("Build failed - build/sim not found")
        print("Build successful")
    except subprocess.CalledProcessError as e:
        print(f"Build failed with error: {e}")
        raise
    except Exception as e:
        print(f"Build failed with error: {e}")
        raise

def run_sim():
    print("\nRunning simulation...")
    with open(LOG_FILE, 'w') as f:
        subprocess.check_call(['./build/sim'], stdout=f, stderr=subprocess.STDOUT)
    print("Simulation complete")

def analyze():
    """Parse analysis output into a dict of metrics."""
    print("\nAnalyzing results...")
    metrics = {}
    try:
        txt = subprocess.check_output(['python3', ANALYZE, LOG_FILE], text=True)
        print("Analysis script output:")
        print(txt)  # Print the analysis output for debugging
    except subprocess.CalledProcessError as e:
        print(f"Analysis script failed with error: {e}")
        return metrics

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
            # Calculate bandwidth in MB/s
            metrics[f'{cur}_mbps'] = metrics[f'{cur}_mpps'] * PACKET_SIZE_BYTES
            cur = None
            continue
        if line.startswith('Direct bus'):
            metrics['duty_direct'] = float(line.split(':')[1].strip().rstrip('%'))
            continue
        if line.startswith('Hybrid bus'):
            metrics['duty_hybrid'] = float(line.split(':')[1].strip().rstrip('%'))
            continue

    print("Parsed metrics:")
    print(metrics)
    return metrics

def sanity_ok():
    rc = subprocess.call(['python3', SANITY, LOG_FILE])
    return rc == 0

def sweep():
    results = []
    print('\nLatency Stall%  Ready(MB/s)  Credit(MB/s)  Ratio_to_Direct')
    print('-'*55)
    
    # Sweep through parameters
    for latency, stall in itertools.product(LATENCIES, STALL_PCTS):
        print(f"\nTesting latency={latency}, stall={stall}%")
        try:
            set_noc_params(latency, stall)
            clean_build()  # Clean build to ensure changes take effect
            run_sim()

            sane = sanity_ok()
            if not sane:
                print("Sanity check failed")
                results.append((latency, stall, None, None, None, None, None, 'DROP'))
                continue

            m = analyze()
            if not m:
                print("No metrics found in analysis")
                results.append((latency, stall, None, None, None, None, None, 'PARSE_ERR'))
                continue

            # Get Direct topology bandwidth from the same simulation run
            direct_bw = m.get('credit_mbps', 0)  # Credit path is the Direct topology
            rxtx_bw = m.get('ready_mbps', 0)     # Ready path is the RXTX topology

            if direct_bw == 0 or rxtx_bw == 0:
                print(f"Zero bandwidth detected - direct: {direct_bw}, rxtx: {rxtx_bw}")
                results.append((latency, stall, rxtx_bw, direct_bw, 0.0, None, None, 'ZERO_BW'))
                continue

            ratio = rxtx_bw / direct_bw
            status = 'OK' if ratio >= TARGET_RATIO else 'FAIL'  # Pass if ratio is >= 50%
            print(f"{latency:7d} {stall:7d}   {rxtx_bw:8.2f}    {direct_bw:8.2f}    {ratio:5.2f} {status}")
            results.append((latency, stall,
                           rxtx_bw, direct_bw,
                           ratio,
                           m.get('duty_direct'), m.get('duty_hybrid'),
                           status))
        except Exception as e:
            print(f"Error testing latency={latency}, stall={stall}%: {e}")
            results.append((latency, stall, None, None, None, None, None, f'ERROR: {str(e)}'))
            continue

    # write CSV report
    report_path = 'noc_sweep_report.csv'
    print(f"\nWriting results to {report_path}")
    print(f"Number of results: {len(results)}")
    
    try:
        with open(report_path, 'w', newline='') as csvfile:
            writer = csv.writer(csvfile)
            writer.writerow(['latency','stall_pct',
                           'rxtx_mbps','direct_mbps',
                           'ratio_to_direct',
                           'duty_direct_%','duty_hybrid_%',
                           'status'])
            for row in results:
                writer.writerow(row)
                print(f"Wrote row: {row}")
        print(f"Successfully wrote {len(results)} rows to {report_path}")
    except Exception as e:
        print(f"Error writing CSV file: {e}")
        # Try to write a backup file
        backup_path = f'noc_sweep_report_backup_{int(time.time())}.csv'
        print(f"Attempting to write backup file: {backup_path}")
        with open(backup_path, 'w', newline='') as csvfile:
            writer = csv.writer(csvfile)
            writer.writerow(['latency','stall_pct',
                           'rxtx_mbps','direct_mbps',
                           'ratio_to_direct',
                           'duty_direct_%','duty_hybrid_%',
                           'status'])
            for row in results:
                writer.writerow(row)

def main():
    sweep()

if __name__ == '__main__':
    main() 