from flask import Flask, render_template, abort
from waitress import serve
import pandas as pd
import json
import os
import numpy as np

app = Flask(__name__)

# FILE PATHS
CSV_PATH = '/app/data.csv'
STATUS_PATH = '/app/status.json'
METRICS_PATH = '/app/metrics.json'

def load_json_safe(path):
    """
    Reads a JSON file safely. 
    Returns an empty dict if the file is missing, empty, or locked.
    This prevents the app from crashing during file writes by other processes.
    """
    if not os.path.exists(path):
        return {}
    try:
        with open(path, 'r') as f:
            content = f.read().strip()
            if not content:
                return {}
            return json.loads(content)
    except Exception as e:
        print(f"[WARNING] Error reading {path}: {e}")
        return {}

def calculate_sharpe(df):
    """Calculates Annualized Sharpe Ratio based on historical CSV data."""
    if df.empty or len(df) < 2 or 'equity' not in df.columns:
        return 0.0
    
    # Sort by date to ensure correct returns calculation
    df = df.sort_values('timestamp')
    df['returns'] = df['equity'].pct_change()
    std_dev = df['returns'].std()
    
    # Annualized Sharpe (assuming daily data approx, scaled by sqrt(365))
    if std_dev > 0:
        return (df['returns'].mean() / std_dev) * np.sqrt(365)
    return 0.0

@app.route('/live-quant-strategy-doge')
def index():
    # 1. READ ONLY OPERATION (Display Layer)
    # The dashboard does not calculate critical metrics; it only reads them.
    account = load_json_safe(STATUS_PATH)
    historical_metrics = load_json_safe(METRICS_PATH)
    
    # 2. Inject Historical Metrics (calculated by the Watcher service)
    account['max_dd'] = historical_metrics.get('max_drawdown', 0.0)
    
    # 3. Process CSV for Trade History and Sharpe Ratio
    trades = []
    sharpe = 0.0
    try:
        if os.path.exists(CSV_PATH):
            df = pd.read_csv(CSV_PATH)
            if not df.empty and 'shares_held' in df.columns:
                # Reconstruct historical equity curve
                df['equity'] = df['cash_available'] + (df['shares_held'] * df['price'])
                sharpe = calculate_sharpe(df)
                
                # Sort and get last 50 trades for the table
                df = df.sort_values('timestamp')
                trades = df.tail(50).iloc[::-1].to_dict('records')
    except Exception as e:
        print(f"[ERROR] CSV processing error: {e}")

    account['sharpe'] = sharpe

    # 4. Visual Percentage Calculations
    current_equity = account.get('equity', 0)
    last_equity = account.get('last_equity', 100000)
    initial_capital = 100000.0
    
    # Daily Return
    if last_equity > 0:
        daily_pct = ((current_equity - last_equity) / last_equity) * 100
    else:
        daily_pct = 0.0
        
    # Total Return
    total_pct = ((current_equity - initial_capital) / initial_capital) * 100

    account['daily_pct'] = daily_pct
    account['total_pct'] = total_pct

    return render_template('index.html', account=account, trades=trades)

@app.route('/')
def home():
    # Security: Obfuscate the entry point
    return "Access Denied", 403

if __name__ == '__main__':
    # Use Waitress as a production-ready WSGI server
    serve(app, host='0.0.0.0', port=5000)
