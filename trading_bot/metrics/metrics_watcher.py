import json
import time
import os
import sys

# FILE PATHS (Absolute paths for Docker environment)
STATUS_PATH = '/app/status.json'
METRICS_PATH = '/app/metrics.json'

# CONFIGURATION
INITIAL_CAPITAL = 100000.0
CHECK_INTERVAL = 1.0  # Seconds to wait between checks

def load_json(path):
    """Safely loads a JSON file."""
    try:
        with open(path, 'r') as f:
            return json.load(f)
    except Exception as e:
        print(f"[ERROR] Reading {path}: {e}")
        return None

def save_json(path, data):
    """Saves data to JSON and logs the update."""
    try:
        with open(path, 'w') as f:
            json.dump(data, f, indent=4)
        print(f"[INFO] Metrics updated: HWM={data['high_water_mark']:.2f}, DD={data['max_drawdown']:.2f}%")
    except Exception as e:
        print(f"[ERROR] Saving {path}: {e}")

def get_file_mtime(path):
    """Returns the last modification time of a file."""
    try:
        return os.stat(path).st_mtime
    except FileNotFoundError:
        return 0

def main():
    print("👀 Starting Metrics Watcher Service...")
    print(f"   Watching: {STATUS_PATH}")
    print(f"   Writing to: {METRICS_PATH}")

    last_mtime = 0

    while True:
        try:
            # 1. Check if status.json has been modified by the C++ Bot
            current_mtime = get_file_mtime(STATUS_PATH)
            
            if current_mtime != last_mtime:
                last_mtime = current_mtime
                
                # 2. Load current data
                status = load_json(STATUS_PATH)
                if not status:
                    continue

                metrics = load_json(METRICS_PATH)
                if not metrics:
                    # Initialize metrics if file doesn't exist
                    metrics = {
                        "high_water_mark": INITIAL_CAPITAL, 
                        "max_drawdown": 0.0
                    }

                # 3. CALCULATION LOGIC
                current_equity = status.get('equity', 0)
                updated = False

                # A. High Water Mark (Historical Equity Peak)
                # It can never be lower than the initial capital
                if current_equity > metrics['high_water_mark']:
                    metrics['high_water_mark'] = current_equity
                    updated = True

                # B. Max Drawdown (Maximum observed loss from peak)
                if metrics['high_water_mark'] > 0:
                    # Calculate percentage drop from HWM
                    current_dd = ((current_equity - metrics['high_water_mark']) / metrics['high_water_mark']) * 100
                else:
                    current_dd = 0.0

                # Update Max DD if current drop is deeper (more negative) than historical record
                if current_dd < metrics['max_drawdown']:
                    metrics['max_drawdown'] = current_dd
                    updated = True

                # 4. Save only if there are significant changes
                if updated:
                    save_json(METRICS_PATH, metrics)

            # Sleep to prevent high CPU usage
            time.sleep(CHECK_INTERVAL)

        except KeyboardInterrupt:
            print("\n[INFO] Stopping watcher.")
            sys.exit(0)
        except Exception as e:
            print(f"[ERROR] Unexpected error in loop: {e}")
            time.sleep(5)

if __name__ == "__main__":
    main()
