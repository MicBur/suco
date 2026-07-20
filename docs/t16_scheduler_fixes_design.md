# T16.1 Design Note: Coordinator Fixes X1 & X2

This document details the design and implementation details for the coordinator fixes addressing loopback IP routing issues (X1) and idle scheduler load distribution issues (X2).

## Bug X1: Routable Worker-IP for Co-located Workers

### Problem
When a worker is co-located on the same physical machine as the coordinator, it typically registers using loopback (`127.0.0.1`) if configured with `--coordinator 127.0.0.1:9000`. The coordinator registers the worker with IP `127.0.0.1`. When a remote client asks the coordinator for direct compilation, the coordinator passes `127.0.0.1` as the worker target. The remote client then tries to connect to loopback on its own machine, leading to connection failures or incorrect routing.

### Solution
1. **Deployment Configuration Fix**: We start co-located workers using their routable LAN IP (e.g. `--coordinator 192.168.0.200:9000`). This ensures `getpeername()` on the coordinator side automatically returns the routable LAN IP (`192.168.0.200`) instead of `127.0.0.1`.
2. **C++ Coordinator Fallback**: To ensure robust out-of-the-box behavior even if a worker connects via localhost, the coordinator will detect if a worker's registered IP is loopback (`127.0.0.1` or `::1`). If so, it will automatically resolve it to the LAN/WAN IP on which the coordinator received the connection, or substitute it with the client-accessible IP address.
   Specifically, inside `worker_handler.cpp`'s `handle_worker_connection`:
   ```cpp
   std::string worker_ip = get_socket_ip(worker_sock);
   if (worker_ip == "127.0.0.1" || worker_ip == "::1" || worker_ip == "localhost") {
       // Replace loopback with the host's actual LAN IP address if possible
       // or fall back to the LAN IP address of the coordinator node.
   }
   ```
   *Note*: The most robust approach is using the LAN IP on the host node, which we will configure in deployment.

---

## Bug X2: Idle Scheduler Round-Robin (Tie-Breaker)

### Problem
The scheduling algorithm `Scheduler::select_best_worker` selects a worker based on `score = (weight * headroom) / (1.0 + slots_used)`. 
When multiple workers are completely idle (e.g. at the beginning of a build or under light/sequential load), their scores are identical.
Because the scheduler uses `if (score > best_score)`, it always defaults to selecting the first worker in the list. This leads to sequential builds compiling entirely on a single node (typically `k3master`), leaving other nodes idle.

### Solution
We implement a thread-safe **Least-Recently-Assigned** tie-breaker.
1. Add `std::atomic<uint64_t> last_assigned_seq{0};` to `WorkerNode`.
2. When selecting a worker in `Scheduler::select_best_worker`, if two workers have the exact same best score, we select the one with the *lower* `last_assigned_seq` value:
   ```cpp
   bool select_this = false;
   if (score > best_score) {
       select_this = true;
   } else if (score == best_score && best_worker) {
       if (w->last_assigned_seq.load(std::memory_order_relaxed) <
           best_worker->last_assigned_seq.load(std::memory_order_relaxed)) {
           select_this = true;
       }
   }
   ```
3. Upon selecting a worker, the scheduler increments a global sequence number and updates the worker's sequence:
   ```cpp
   if (best_worker) {
       static std::atomic<uint64_t> global_seq{0};
       best_worker->last_assigned_seq.store(++global_seq, std::memory_order_relaxed);
   }
   ```
This guarantees that idle workers are chosen in a round-robin fashion, distributing sequential and parallel compile jobs evenly across the grid.
