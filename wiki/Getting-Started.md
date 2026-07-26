# 🚀 Getting Started with SUCO Grid

Getting up and running with SUCO Grid takes less than **30 seconds**.

---

## 📦 Installation

### Linux (Ubuntu / Debian APT Repository)
Install directly from the official SUCO APT repository:

```bash
# 1. Download & register the official GPG keyring
curl -fsSL https://micbur.github.io/suco/suco-archive-keyring.asc | sudo tee /etc/apt/keyrings/suco.asc >/dev/null

# 2. Add the SUCO APT repository source list
echo "deb [signed-by=/etc/apt/keyrings/suco.asc] https://micbur.github.io/suco stable main" | sudo tee /etc/apt/sources.list.d/suco.list >/dev/null

# 3. Update and install
sudo apt update && sudo apt install -y suco
```

### Windows
Download the automated NSIS installer (`suco-0.12.0-windows-x64-setup.exe`) from the [GitHub Releases Page](https://github.com/MicBur/suco/releases).

---

## 🌐 Setting Up a Grid in 3 Steps

### Step 1: Start the Coordinator (Central Hub)
On your primary Linux server or master node, start the coordinator:

```bash
suco-coordinator
```
*The coordinator will immediately start listening for workers on TCP Port 9000, Web Dashboard on Port 9001, and UDP Auto-Discovery on Port 9002.*

### Step 2: Start Workers (Compute Nodes)
On any computer on your network (Linux or Windows):

```bash
suco-worker --coordinator <COORDINATOR_IP>:9000 --slots 8
```
*If UDP Auto-Discovery is active on your subnet, workers can be started without `--coordinator` and will discover the hub automatically.*

### Step 3: Run your First Distributed Build
On your client machine (Windows or Linux):

```bash
suco make -j16
```
Or use the compiler wrapper prefix:

```bash
suco-cl++ -O2 -std=c++20 -c main.cpp -o main.o
```

---

## 🖥️ Live Web Dashboard
Open your browser and navigate to:
`http://<COORDINATOR_IP>:9001`

The dashboard displays:
- Active Worker Nodes and CPU Core gauges
- Real-time job compilation stream
- L2 SSD Cache Hit Rate gauge
- Active compiler toolchain matrix
