from flask import Flask, render_template, jsonify
from waitress import serve
import json
import csv
import os
from datetime import datetime

app = Flask(__name__)

# Configuració de rutes internes del contenidor
BOTS = {
    "curr": {
        "name": "PRODUCCIÓ",
        "path": "/app/data_curr",
        "color": "#00ff88"
    },
    "exp": {
        "name": "EXPERIMENT",
        "path": "/app/data_exp",
        "color": "#bb86fc"
    }
}

def get_bot_data(bot_id):
    path_base = BOTS[bot_id]["path"]
    data = {
        "account": {"equity": 0, "cash": 0, "invested": 0, "total_return_pct": 0, "avg_daily_pct": 0, "max_dd": 0, "sharpe": 0},
        "trades": [],
        "config": BOTS[bot_id]
    }

    # 1. Status
    try:
        with open(os.path.join(path_base, "status.json"), 'r') as f:
            status = json.load(f)
            data["account"]["equity"] = float(status.get('equity', 0))
            data["account"]["cash"] = float(status.get('cash', 0))
            data["account"]["invested"] = data["account"]["equity"] - data["account"]["cash"]
    except: pass

    # 2. Metrics
    try:
        with open(os.path.join(path_base, "metrics.json"), 'r') as f:
            m = json.load(f)
            data["account"]["avg_daily_pct"] = m.get('daily_avg_pct', 0.0)
            data["account"]["sharpe"] = m.get('sharpe_ratio', 0.0)
            data["account"]["total_return_pct"] = m.get('ROI', 0.0)
            data["account"]["max_dd"] = -1 * abs(m.get('max_drawdown', 0.0))
    except: pass

    # 3. Trades
    try:
        csv_path = os.path.join(path_base, "data.csv")
        if os.path.exists(csv_path):
            with open(csv_path, 'r') as f:
                reader = csv.reader(f)
                rows = list(reader)
                if rows and rows[0][0] == 'timestamp': rows = rows[1:]
                for row in reversed(rows):
                    if len(row) < 4: continue
                    try:
                        date_str = datetime.fromtimestamp(float(row[0])).strftime('%Y-%m-%d %H:%M:%S')
                    except: date_str = row[0]
                    data["trades"].append({
                        "timestamp": date_str,
                        "action": row[1],
                        "price": row[2],
                        "qty": row[3],
                        "cash": row[4] if len(row) > 4 else "N/A"
                    })
                    if len(data["trades"]) >= 50: break
    except: pass

    return data

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/api/bot/<bot_id>')
def api_bot(bot_id):
    if bot_id not in BOTS: return jsonify({"error": "Bot no trobat"}), 404
    return jsonify(get_bot_data(bot_id))

if __name__ == '__main__':
    print("🚀 Unified Dashboard iniciat...")
    serve(app, host='0.0.0.0', port=5000)
