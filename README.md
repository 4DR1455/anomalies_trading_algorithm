# HFT-Style Anomalies Trading Algorithm (C++ / OCaml / Python)

**A low-latency, microservices-based trading engine designed to automatically detect market price anomalies of a specific stock.**
This project implements a hybrid architecture where execution, strategy logic, and monitoring run as decoupled processes communicating via standard Linux IPC mechanisms.

<div align="center">
  <a href="http://adria-trading-bot.duckdns.org/live-quant-strategy-doge">
    <img src="./MDmedia/image.png" alt="Dashboard Screenshot" width="75%">
  </a>
  <br>
  <em>(Fig 1: Real-time Dashboard showing automated performance analysis including Sharpe Ratio and Max Drawdown. Note: Data in this screenshot is simulated to demonstrate the statistical engine's capabilities over a longer timeframe. Click to see the live dashboard.)</em>
</div>

## 🏗 System Architecture

The system follows a "Separation of Concerns" principle to maximize stability and minimize latency.

```mermaid
graph TD
    A[Alpaca API] <-->|HTTP/REST| B(C++ Execution Engine);
    B <-->|Linux Pipes| C{OCaml Strategy Brain};
    B -->|Writes JSON| D[Shared Volume / Status File];
    D -->|Reads| E[Python Dashboard];
    E -->|HTML/JS| F[User Browser];
    G[parameters.txt] -.->|Config| C    
    style B fill:#69f,stroke:#333,stroke-width:2px,color:white
    style C fill:#f96,stroke:#333,stroke-width:2px,color:white
    style F fill:#6c9,stroke:#333,stroke-width:2px,color:white
```

## 🔧 Core Components

* **Execution Engine (C++17):** Handles API connectivity (`libcurl`), order management, and safety checks. Optimized for speed and low overhead.
* **Strategy Core (OCaml):** Pure functional logic for market analysis. Isolated from the network layer to ensure deterministic behavior.
* **Inter-Process Communication:** Uses raw **Linux Pipes (`fork()` + `pipe()`)** to keep latency strictly minimal within the container.
* **Analytics Dashboard (Python/Flask/Waitress + NumPy):**
    * Connects to the live data stream.
    * **Automated Statistical Analysis:** Calculates **Sharpe Ratio**, **Max Drawdown**, and **Volatility** in real-time based on the trade history.
    * Provides a responsive UI for monitoring the bot from any device.

## 🧠 Decision Algorithm

The strategy logic operates on a discretized state space to identify mean-reversion opportunities.

1. **Prediction (Signal Processing):** The brain receives the latest market data and calculates the momentum ratio relative to the previous state. It maps this continuous ratio into a discretized **"Range Grid"**. Then compares the current range against an EMA-based prediction model to filter out noise and detect price anomalies.

2. **Decision (Thresholding):** A trade signal is generated if the divergence between the actual market range and the predicted range exceeds the `MinMargin` parameter. 
   * **Sell Signal:** Market reality is `MinMargin` or more above prediction.
   * **Buy Signal:** Market reality is `MinMargin` or more below prediction.

3. **Position Sizing (Asymptotic Allocation):** Instead of fixed lot sizes, the system dynamically calculates the optimal position size using a non-linear asymptotic formula. This allows the suer to define the strategy, manually depending on the stock, you can define an agressive entry strategy, that reacts strong at every minimum anomaly, a sniper-style strategy that waits for a big anomaly to take advantage of it with all the budget, or a linear buying one; it all depends on the $level$ value.
   
   The allocation formula is:

   $$SharesAmount(x) = Max \times \left(1 - (1 - x^{level})^{\frac{1}{level}}\right)$$

   *Where:*
   * $x$: The normalized deviation ratio ($CurrentRange / MaxRange$), with $0 \leq x \leq 1$.
   * $Max$: Maximum amount of shares allowed at the operation.
   * $level$: Convexity parameter (customizable in `parameters.txt`).

   **Interactive Visualization:**
   Understand how the `level` parameter affects the capital allocation curve by interacting with the graph below:

   <div align="center" style="margin-left: 15px;">
      <a href="https://www.desmos.com/calculator/lsct6txxp1">
        <img src="./MDmedia/grafic.gif" alt="Interactive Graphic" width="75%">
      </a>
      <br>
      <em>(Click to open interactive graphic)</em>
   </div>

### Algorythm Diagram
```mermaid
graph LR
    A([Wait for Market Data]) --> B[Receive Price & Calculate Range Ratio];
    B --> C[Compute Prediction based on EMA];
    C --> D{Anomaly Detected?};
    
    D -- No (Diff < MinMargin) --> A;
    D -- Yes (Diff > MinMargin) --> E{Direction?};

    E -- Overvalued --> F[SELL Signal];
    E -- Undervalued --> G[BUY Signal];

    F --> H[Calculate Size: SharesAmount];
    G --> H;

    H --> I[Execute Order via Alpaca];
    I --> A;

    style G fill:#28a745,stroke:#333,stroke-width:2px,color:white
    style F fill:#dc3545,stroke:#333,stroke-width:2px,color:white
    style D fill:#f96,stroke:#333,stroke-width:2px,color:white
    style E fill:#f96,stroke:#333,stroke-width:2px,color:white
```

## 🚀 Key Engineering Features

1.  **Self-Correcting Order Management:** The C++ engine handles partial fills and network timeouts autonomously without crashing the strategy logic.
2.  **Containerized Security:** Runs in a hardened Docker container with a non-root user. API keys are injected via environment variables, never stored on disk.
3.  **Dynamic Performance Analysis:** Unlike static backtests, the system continuously evaluates its own performance metrics (P&L, Risk Ratios) using the `numpy` engine integrated into the web server.

# 🚀 High-Frequency Doge Trading Bot

A quantitative trading system engineered for high performance and security. It combines the raw speed of **C++** for execution, the mathematical safety of **OCaml** for strategy logic, and a secure **Python** analytics dashboard.

## 🏗️ Project Architecture

The system follows a secure **microservices architecture** managed by Docker Compose:

1.  **Bot Core (C++ / OCaml):** * Executes trades via Alpaca API.
    * Writes real-time status to `status.json`.
    * *Privileges:* Read/Write on data.
2.  **The Accountant (Python Watcher):** * Background process that calculates High Water Mark and Max Drawdown.
    * Updates `metrics.json` only when status changes.
    * *Privileges:* Write access to metrics.
3.  **The Dashboard (Python Flask):** * Visualizes performance and trades.
    * **Security:** Runs in a **Read-Only** container with a restricted user (UID 1001). It cannot modify code or data even if compromised.

## 📂 Project Structure

```text
.
├── hands_api.cc           # C++ Execution Engine (Network & Order Management)
├── brain7_2.ml            # OCaml Strategy Core (Math & Logic)
├── docker-compose.yml     # Service Orchestrator (Microservices Definition)
├── Makefile               # Build automation
├── parameters.txt         # Runtime strategy configuration
│
├── dashboard/             # SECURE VISUALIZATION LAYER (User 1001)
│   ├── app.py             # Read-Only Flask Backend
│   ├── Dockerfile         # Multi-stage build definition
│   ├── templates/         # Frontend UI
│   └── metrics.json       # Persisted metrics storage
│
└── metrics/               # CALCULATION LAYER (User 1000)
    └── metrics_watcher.py # Background service for calculating Drawdown
```

## 🛠️ How to Run

### Prerequisites
* **Docker & Docker Compose** (Plugin v2 recommended).
* **Alpaca Markets API Key** (Paper or Live).

### 1. Setup Environment
Clone the repository and export your API keys in your terminal:

```bash
export APCA_API_KEY_ID="your_alpaca_key"
export APCA_API_SECRET_KEY="your_alpaca_secret"
```

### 2. Initialize Data Files & Permissions
Create the necessary JSON files and set permissions. This step is **critical** for the secure microservices architecture (ensuring the Watcher can write while the Web can only read).

```bash
# Create files if they don't exist yet to prevent Docker mount errors
touch status.json
touch dashboard/metrics.json

# Allow read/write access for the Docker users (UID 1000 & 1001)
# This enables the secure 'bot-watcher' to update metrics.
chmod 664 dashboard/metrics.json
```

### 3. Build & Run
Deploy the entire stack (Core, Web, and Watcher) with a single command. Docker Compose will handle the build and network creation automatically.

```bash
docker compose up -d --build
```

### 4. Access the Dashboard
Open your web browser and navigate to:
[http://localhost:80/live-quant-strategy-doge](http://localhost:80/live-quant-strategy-doge)

---
*Note: This is a live project running on Oracle Cloud Infrastructure. You can monitor its performance live [here](http://adria-trading-bot.duckdns.org/live-quant-strategy-doge). 

**Why doge?** I wanted a volatile stock to generate more actions to see the bot working.*

*Disclaimer: This software is for educational purposes only. Do not risk capital you cannot afford to lose.*
