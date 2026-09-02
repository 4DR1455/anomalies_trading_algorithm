"""
======================================================================================
THE ACCOUNTANT (METRICS WATCHER)
======================================================================================
Background service that monitors the bot's financial performance.
* Responsibilities:
1. High Water Mark (HWM) & Max Drawdown Calculation.
2. Daily Performance Snapshot (last_100.csv).
3. Advanced Statistical Analysis (Sharpe Ratio, Daily Returns).
4. Data Persistence & Cleanup (Metrics JSON, Historical CSVs).
======================================================================================
"""

import json
import time
import os
import csv
import math
import statistics
from datetime import datetime

# --- DIRECTORY CONFIGURATION ---
DATA_DIR = '/app'
os.makedirs(DATA_DIR, exist_ok=True)

# Input Paths (Read-Only)
STATUS_PATH = '/app/status.json' # Information source
INI_WEALTH_PATH = '/app/ini_w.txt' # Initial capital (e.g., 100000)

# Output Paths (Read-Write)
METRICS_PATH = os.path.join(DATA_DIR, 'metrics.json')
HISTORY_PATH = os.path.join(DATA_DIR, 'equity_history.json')
DAILY_CSV_PATH = os.path.join(DATA_DIR, 'last_100.csv')
DATA_CSV_PATH = os.path.join(DATA_DIR, 'data.csv')

# --- UTILITY FUNCTIONS ---

def load_json(path):
    """Safely loads a JSON file, returning None on failure."""
    if os.path.exists(path):
        try:
            with open(path, 'r') as f:
                content = f.read()
                if not content: return None
                return json.loads(content)
        except (json.JSONDecodeError, IOError):
            return None
    return None

def save_json(path, data):
    """
    Saves data to a JSON file. 
    Note: Standard writing used instead of atomic to avoid 'Permission Denied' 
    on directory temporary file creation in some Docker environments.
    """
    try:
        with open(path, 'w') as f:
            json.dump(data, f, indent=4)
    except Exception as e:
        print(f"[ERROR] Error al desar {path}: {e}")

def cleanup_data_csv(limit=60):
    """
    Maintenance Task: Keeps the data.csv file lean by preserving only the last N entries.
    This optimizes Dashboard loading times.
    """
    if not os.path.exists(DATA_CSV_PATH):
        return
    
    try:
        with open(DATA_CSV_PATH, 'r', newline='') as f:
            reader = csv.reader(f)
            all_rows = list(reader)

        if not all_rows or len(all_rows) <= limit + 1:
            return

        header = all_rows[0]
        rows_to_keep = all_rows[-limit:]
        
        with open(DATA_CSV_PATH, 'w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(header)
            writer.writerows(rows_to_keep)
            
        print(f"[MANTENIMENT] data.csv optimitzat: mantingudes les últimes {limit} files.")
                
    except Exception as e:
        print(f"[ERROR] cleanup_data_csv ha fallat: {e}")

def update_daily_csv(current_equity):
    """Captures a daily equity snapshot at 12:00 PM for historical analysis."""
    now = datetime.now()
    today_str = now.strftime('%Y-%m-%d')
    header = ['date', 'equity']
    rows = []
    
    if os.path.exists(DAILY_CSV_PATH):
        try:
            with open(DAILY_CSV_PATH, 'r', newline='') as f:
                reader = csv.reader(f)
                rows = list(reader)
        except: pass

    if not rows: rows.append(header)
    already_recorded_today = any(row[0] == today_str for row in rows)

    if not already_recorded_today and now.hour >= 12:
        rows.append([today_str, str(current_equity)])
        if len(rows) > 101: 
            rows = [rows[0]] + rows[-100:]
            
        try:
            with open(DAILY_CSV_PATH, 'w', newline='') as f:
                writer = csv.writer(f)
                writer.writerows(rows)
            print(f"[SNAPSHOT] Patrimoni diari registrat: {current_equity}")
        except Exception as e:
            print(f"[ERROR] Error al CSV diari: {e}")
            
    return rows

def calculate_advanced_metrics(daily_rows):
    """Calculates Average Daily Return % and Annualized Sharpe Ratio."""
    data_rows = [r for r in daily_rows if r[0] != 'date']
    if len(data_rows) < 2: return 0.0, 0.0
    
    try:
        equities = [float(row[1]) for row in data_rows]
        returns = [(equities[i] - equities[i-1]) / equities[i-1] for i in range(1, len(equities)) if equities[i-1] > 0]
                
        if not returns: return 0.0, 0.0
        
        avg_daily_return = statistics.mean(returns)
        stdev_daily = statistics.stdev(returns) if len(returns) > 1 else 0.0
        
        sharpe = (avg_daily_return / stdev_daily) if stdev_daily > 0 else 0.0 # Without anualizer (daily sharpe)
        return avg_daily_return * 100, sharpe
        
    except:
        return 0.0, 0.0

# --- MAIN LOOP ---

def main():
    print("[SISTEMA] El Comptable (Watcher) iniciat. Monitoritzant rendiment...")
    
    initial_wealth = 0.0
    if os.path.exists(INI_WEALTH_PATH):
        try:
            with open(INI_WEALTH_PATH, 'r') as f:
                content = f.read().strip()
                if content: initial_wealth = float(content)
            print(f"[CONFIG] Capital inicial detectat: {initial_wealth}$")
        except:
            print("[AVÍS] No s'ha pogut llegir ini_w.txt.")

    # 1. Initialize Metrics with persistence protection
    metrics = {
        "high_water_mark": 0.0,
        "max_drawdown": 0.0,
        "ROI": 0.0,
        "daily_avg_pct": 0.0, 
        "sharpe_ratio": 0.0,
        "last_update": ""
    }

    saved_metrics = load_json(METRICS_PATH)
    if saved_metrics:
        for key in ["high_water_mark", "max_drawdown", "ROI", "daily_avg_pct", "sharpe_ratio"]:
            if key in saved_metrics:
                metrics[key] = saved_metrics[key]
        print(f"[SISTEMA] Mètriques històriques carregades: MaxDD {metrics['max_drawdown']}%")

    last_processed_timestamp = ""

    while True:
        try:
            status_data = load_json(STATUS_PATH)
            
            if status_data and 'equity' in status_data and 'timestamp' in status_data:
                if status_data['timestamp'] == last_processed_timestamp:
                    time.sleep(10)
                    continue

                current_equity = float(status_data['equity'])
                current_cash = float(status_data.get('cash', 0.0))
                now = time.time()

                # API Glitch Filter: Ignore invalid states
                if current_cash <= 0:
                    last_processed_timestamp = status_data['timestamp']
                    continue

                if current_equity > 0:
                    # A. High Water Mark & Strictly Increasing Max Drawdown
                    if current_equity > metrics["high_water_mark"]:
                        metrics["high_water_mark"] = current_equity
                    
                    hwm = metrics["high_water_mark"]
                    if hwm > 0:
                        current_dd = ((hwm - current_equity) / hwm) * 100
                        if current_dd > metrics["max_drawdown"]:
                            metrics["max_drawdown"] = round(current_dd, 2)
                            print(f"[AVÍS] Nou Max Drawdown detectat: {metrics['max_drawdown']}%")

                    # B. ROI & Statistical Analysis
                    if initial_wealth > 0:
                        metrics["ROI"] = round(((current_equity - initial_wealth) / initial_wealth) * 100, 2)

                    daily_rows = update_daily_csv(current_equity)
                    daily_avg, sharpe = calculate_advanced_metrics(daily_rows)
                    metrics["daily_avg_pct"] = round(daily_avg, 2)
                    metrics["sharpe_ratio"] = round(sharpe, 3)

                    # C. History Chart Persistence
                    history = load_json(HISTORY_PATH) or []
                    history.append({"time": now, "equity": current_equity})
                    history = [x for x in history if x['time'] > (now - 90000)]
                    save_json(HISTORY_PATH, history)

                    # D. Maintenance: Keep data.csv small (last 60 trades)
                    cleanup_data_csv(60)

                    # E. Save Final Metrics
                    metrics["last_update"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                    save_json(METRICS_PATH, metrics)
                
                last_processed_timestamp = status_data['timestamp']

            time.sleep(25)

        except Exception as e:
            print(f"[CRÍTIC] Error al bucle principal: {e}")
            time.sleep(30)

if __name__ == "__main__":
    main()
