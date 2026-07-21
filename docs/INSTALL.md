# SUCO — Installation & Setup Guide

This guide takes you from zero to a working compile grid: install SUCO, start a
coordinator and one or more workers, point your builds at the grid, and verify it.

- **Coordinator** — one per grid. Assigns jobs, hosts the shared cache and the web dashboard.
- **Worker** — one per compile machine. Registers with the coordinator and compiles jobs.
- **Client** — any machine that runs builds. Ships preprocessed source to the grid.

A single machine can be all three. A typical grid is one *head node* (coordinator + worker)
plus a few *compile nodes* (worker only), with developers building from their own machines.

---

## 1. Requirements

- Linux (x86-64), Debian/Ubuntu-based for the `apt` path.
- A C/C++ toolchain on every worker (`g++`/`gcc`, optionally `clang`). Workers should have the
  **same compiler version** for byte-identical, cacheable results.
- All machines on the same LAN. Open these ports between them:

  | Port | Proto | On | Purpose |
  |------|-------|-----|---------|
  | 9000 | TCP | coordinator | job queries / control |
  | 9001 | TCP | coordinator | web dashboard (optional) |
  | 9002 | UDP | coordinator | worker auto-discovery |
  | 9005 | TCP | workers | direct client→worker dispatch |

---

## 2. Install

### Option A — `apt` (recommended)

```bash
curl -fsSL https://micbur.github.io/suco/suco-archive-keyring.asc \
  | sudo tee /etc/apt/keyrings/suco.asc >/dev/null
echo "deb [signed-by=/etc/apt/keyrings/suco.asc] https://micbur.github.io/suco stable main" \
  | sudo tee /etc/apt/sources.list.d/suco.list >/dev/null
sudo apt update && sudo apt install suco
suco --version
```

Run these on **every** machine in the grid. Installing does **not** start anything — a fresh
install never silently joins a running grid.

### Option B — build from source

```bash
sudo apt install -y build-essential cmake ninja-build libssl-dev libzstd-dev
git clone https://github.com/MicBur/suco.git && cd suco
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
( cd build && cpack -G DEB )        # produces build/suco_*_amd64.deb
sudo apt install ./build/suco_*_amd64.deb
```

---

## 3. Start the grid

Nothing autostarts; you choose each machine's role with systemd.

### Head node (coordinator + a worker)

```bash
sudo systemctl enable --now suco-coordinator suco-worker
```

The coordinator listens on 9000/9001/9002. The dashboard is now at `http://<this-host>:9001`.

### Each compile node (worker only)

```bash
sudo systemctl enable --now suco-worker
```

Workers **auto-discover** the coordinator on the LAN via UDP (port 9002) — no configuration needed
if broadcast reaches them. If discovery is blocked (routed subnets, cloud), point the worker at the
coordinator explicitly:

```bash
sudo systemctl edit suco-worker
# add:
#   [Service]
#   ExecStart=
#   ExecStart=/usr/bin/suco-worker --coordinator <coordinator-ip>:9000
sudo systemctl restart suco-worker
```

Slots default to the machine's CPU count. To reserve cores, append `--slots N` the same way.

### Client machines (where you run builds)

Tell the client where the coordinator is — pick one:

```bash
suco setup                                  # interactive: host, port, verify
# — or set it directly:
echo "coordinator_host=<coordinator-ip>" | sudo tee /etc/suco/suco.conf
# — or per-shell:
export SUCO_COORDINATOR_HOST=<coordinator-ip>
```

---

## 4. Verify

```bash
suco doctor      # full health check: connectivity, workers, toolchains, cache
suco status      # live grid state (workers, slots, load)
```

`suco doctor` should list every worker with green checks. The dashboard at
`http://<coordinator>:9001` shows workers, cache hit rate, and live compile history.

---

## 5. Use it in a build

SUCO is a build wrapper — put `suco` in front of your build command and it redirects `CC`/`CXX`
to the grid:

```bash
suco make -j32
suco ninja
suco cmake --build build
```

Or wire it in explicitly:

```bash
# CMake — set the compiler launcher:
cmake -B build -DCMAKE_CXX_COMPILER_LAUNCHER=suco-cl++ -DCMAKE_C_COMPILER_LAUNCHER=suco-cl
# or in CMakeLists.txt:
#   set(CMAKE_CXX_COMPILER_LAUNCHER suco-cl++)

# Make / anything honoring CC/CXX:
export CC=suco-cl CXX=suco-cl++
```

The **first** build is a cold build (fans out across the grid). Every **rebuild** of unchanged
files is served from the cache — that is where SUCO is dramatically faster.

More: `suco run <cmd>` runs any command on a grid worker; `suco test` runs a test suite
distributed and cached (unchanged tests don't re-run). See the [README](../README.md).

---

## 6. Optional — grid authentication

By default any host on the LAN can submit jobs. To require a shared secret, set the **same**
`SUCO_SECRET` on the coordinator, every worker, and every client:

```bash
sudo systemctl edit suco-coordinator     # and suco-worker
# add:
#   [Service]
#   Environment=SUCO_SECRET=<a-long-random-string>
sudo systemctl restart suco-coordinator suco-worker
# clients:
export SUCO_SECRET=<the-same-string>
```

Clients without the secret are rejected. (`SUCO_TLS=1` additionally encrypts the transport.)

---

## 7. Cache

The coordinator keeps a content-addressed SSD cache (default ~5 GB, LRU). Clear it grid-wide with:

```bash
suco cache clear
```

---

## 8. Troubleshooting

| Symptom | Fix |
|---|---|
| `suco doctor`: no workers | Workers can't reach the coordinator. Check port 9002 (UDP) or set `--coordinator <ip>:9000` on the worker. |
| Builds compile locally, grid idle | Client isn't pointed at the coordinator — run `suco setup` or set `coordinator_host`. |
| `Failed to read cache response` | Auth mismatch — `SUCO_SECRET` differs between client and grid. |
| Workers online but no cache hits | Compiler versions differ across nodes — align them for byte-identical output. |

---

## 9. Upgrade / uninstall

```bash
sudo apt update && sudo apt upgrade suco          # upgrade (repeat per node)
sudo systemctl disable --now suco-worker suco-coordinator
sudo apt remove suco
```
