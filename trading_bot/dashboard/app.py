from flask import Flask, render_template, abort
from waitress import serve
import pandas as pd
import json
import os

app = Flask(__name__)

# CONFIGURATION
# No API Keys needed here. Maximum Security.
CSV_PATH = '/app/data.csv'
STATUS_PATH = '/app/status.json'

def get_local_status():
    """Reads status written by the C++ bot"""
    try:
        with open(STATUS_PATH, 'r') as f:
            return json.load(f)
    except:
        # File doesn't exist yet or read error
        return {"equity": 0.0, "cash": 0.0, "invested": 0.0}

@app.route('/live-quant-strategy-doge')
def index():
    account = get_local_status()
    
    # --- NEW PERFORMANCE CALCULATIONS ---
    current_equity = account.get('equity', 0)
    last_equity = account.get('last_equity', 100000) # Fallback to 100k
    initial_capital = 100000.0

    # 1. Daily P&L (Percent Change)
    if last_equity > 0:
        daily_pct = ((current_equity - last_equity) / last_equity) * 100
    else:
        daily_pct = 0.0

    # 2. Total Return
    total_pct = ((current_equity - initial_capital) / initial_capital) * 100

    # Pass data to HTML
    account['daily_pct'] = daily_pct
    account['total_pct'] = total_pct
    # ---------------------------------

    try:
        df = pd.read_csv(CSV_PATH)
        trades = df.tail(50).iloc[::-1].to_dict('records')
    except:
        trades = []

    return render_template('index.html', account=account, trades=trades)

@app.route('/') 
def home():
    return "Access Denied", 403

if __name__ == '__main__':
    serve(app, host='0.0.0.0', port=5000)