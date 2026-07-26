# ❓ Troubleshooting & Frequently Asked Questions

---

## 🔒 Required Firewall Ports

Ensure the following ports are open on your local subnet:

| Port | Protocol | Purpose |
| :--- | :--- | :--- |
| **9000** | TCP | Coordinator Control & Job Query |
| **9001** | TCP | Web Dashboard & Prometheus Metrics |
| **9002** | UDP | Auto-Discovery Broadcast |
| **9005** | TCP | Worker Direct Dispatch Compilation Listener |

---

## 🛡️ Circuit Breaker Mechanism (#14)

If the coordinator becomes unreachable, SUCO's process-wide Circuit Breaker triggers after 2 consecutive connection failures:
- **Fast-Fail**: Bypasses network attempts for 30 seconds.
- **Local Fallback**: Automatically falls back to local CPU compilation.
- **Fail-Safe Guarantee**: Ensures builds never fail due to network outages.

---

## 🔑 Security & Authentication (`SUCO_SECRET`)

To secure your grid from unauthorized clients, set `SUCO_SECRET` in your environment or system configuration:

```bash
export SUCO_SECRET="your_shared_grid_passphrase"
```

Both coordinator and worker nodes must share the same `SUCO_SECRET`.
