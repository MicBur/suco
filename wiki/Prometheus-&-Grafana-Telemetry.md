# 📊 Prometheus & Grafana Telemetry

SUCO Coordinator exposes an open HTTP Prometheus metrics exporter on **Port 9001**.

---

## 📈 Endpoint URL
`http://<COORDINATOR_IP>:9001/metrics`

---

## 📊 Metrics Schema

| Metric Name | Type | Description |
| :--- | :--- | :--- |
| `suco_active_workers` | Gauge | Number of active worker nodes currently connected to the coordinator |
| `suco_active_worker_slots` | Gauge | Total available compilation CPU slots across the grid |
| `suco_cache_hits_total` | Counter | Total L2 SSD cache hits since coordinator startup |
| `suco_cache_misses_total` | Counter | Total L2 SSD cache misses since coordinator startup |
| `suco_jobs_completed_total` | Counter | Total translation units successfully compiled |
| `suco_job_duration_seconds` | Histogram | Distribution of compilation task durations |

---

## ⚙️ Prometheus Configuration (`prometheus.yml`)

```yaml
scrape_configs:
  - job_name: 'suco_grid'
    static_configs:
      - targets: ['192.168.0.200:9001']
```
