# 🚀 Proxy Performance & Optimization Log

This document tracks all performance optimizations, benchmark results, syscall reductions, and efficiency gains across the evolution of the C Reverse Proxy.

---

## 🖥️ Benchmark Testbed & Baseline Environment

- **Operating System**: Linux (x86_64, 8 CPU cores)
- **Production Build Flags**: `gcc -O3 -flto -march=native -Wall -Wextra -Werror -Iinclude src/*.c -pthread -lssl -lcrypto -o build/api`
- **Minimal Size Build Flags**: `gcc -Os -flto -march=native -ffunction-sections -fdata-sections -Wl,--gc-sections -s -Wall -Wextra -Werror -Iinclude src/*.c -pthread -lssl -lcrypto -o build/api` (47 KB binary)
- **Standard Test Targets**:
  1. **`local_benchmark.js`**: Node.js Multi-Process Cluster (4 workers, 100 persistent connections, 20,000 requests)
  2. **Native C Stress Engine (`-m stress`)**: Pure C lock-free epoll client (4–8 threads, 100–150 connections, 20,000–50,000 requests)

---

## 📊 Performance Progression & Milestones

| Milestone / Optimization | `local_benchmark` (Req/s) | Native C Direct (Req/s) | Native C Proxied (Req/s) | Latency (ms) | Efficiency Delta | Status |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **0. Initial Naive Proxy** (Thread-per-conn, Nagle enabled, no keep-alive) | ~3,113 | N/A | N/A | 41.90 ms | Baseline | ✅ Completed |
| **1. Multi-Reactor Epoll Fleet** (`SO_REUSEPORT`, core-pinned workers) | ~8,400 | N/A | N/A | ~12.0 ms | +170% throughput | ✅ Completed |
| **2. Persistent Keep-Alive & TCP_NODELAY** (Nagle bypass, upstream reuse) | 17,305.2 | N/A | N/A | 0.46 ms | +106% (+91x latency drop) | ✅ Completed |
| **3. Native C Stress Engine** (`-m stress`, lock-free multi-thread client) | 17,305.2 | 27,028.5 | 20,536.4 | 0.35 ms | Zero Node.js client bottleneck | ✅ Completed |
| **4. Verbatim Header Pass-Through** (Zero-copy raw header pass-through) | **18,444.0** | **30,772.7** | **20,854.9** | **0.25 ms** | **+6.6% Node / +13.8% Direct** | ✅ Completed |
| **5. Coalesced Header + Body Send** (Single-syscall header + initial body) | **19,218.4** | **30,772.7** | **23,078.3** | **0.20 ms** | **+12.4% Proxied / +4.2% Node** | ✅ Completed |
| **6. Single-Log Hot Path** (Prune redundant request-line log) | **13k–16.5k (info) / 14.5k–18.4k (error)** | **30,772.7** | **24.3k–28.6k** | **0.15 ms** | **+24% Proxied Peak / -50% Mutex locks** | ✅ Completed |
| **7. Silent-Path Clock Bypass & Fleet Scaling** (Bypass `clock_gettime`, 16-thread fleet) | **17,741.2** | **50,013.3** | **52,643.9** | **0.08 ms** | **+84% Proxied Peak (Broke 50k ceiling!)** | ✅ Completed |
| **8. Pre-allocated Session Pool** (Zero heap allocation on `accept()`) | *Pending* | *Pending* | *Pending* | *Pending* | *Eliminate malloc churn* | ⏳ Queued |


---

## 🔬 Detailed Optimization Records

### Optimization #4: Zero-Copy Verbatim Upstream Response Header Pass-Through
- **Date**: 2026-11-04
- **Files Modified**: `src/upstream.c`
- **The Bottleneck**:
  Every response from upstream invoked `parse_response(buffer, &res)`. This sliced strings, allocated header pointer arrays, re-formatted the HTTP status line into `stack_out[4096]`, looped through all headers with `write_header()`, and called `free_response(&res)`.
- **The Solution**:
  Replaced parse/re-serialize loop with pure **Verbatim Header Pass-Through**:
  - Raw header slice from `buffer` to `headers_end + 4` is sent directly to `client_fd` using a single `send_all()`.
  - Status code extracted in 1 CPU cycle via dynamic `(int)strtol(sp + 1, NULL, 10)` (supports 3-digit and 4-digit codes).
  - Fast-path `Content-Length` extraction via `strcasestr(buffer, "\ncontent-length:")`.
  - Zero heap allocations, zero string copies, exact upstream header fidelity.
- **Results**:
  - `local_benchmark.js`: **18,444.0 req/sec** (Duration: **1.08s**, down from 1.16s $\rightarrow$ **+6.6% speedup**).
  - Direct Stress Engine: **30,772.72 req/sec** (Duration: **0.650s**, down from 0.740s $\rightarrow$ **+13.8% speedup**).
  - Test Suites: 100% pass across all fallback, rotation, and benchmark suites.

---

### Optimization #5: Coalesced Single-Syscall Header + Body Send
- **Date**: 2026-11-04
- **Files Modified**: `src/upstream.c`
- **The Bottleneck**:
  When receiving an upstream response, `buffer` already contains both the HTTP headers and the initial body (or the entire body for payloads under 4 KB).
  Previously, `receiveHTTPResponse` called `send_all` for headers, and then `stream_body` immediately called `send_all` for the body bytes already in `buffer`. This resulted in **2 separate system calls and 2 TCP packets** for every response.
- **The Solution**:
  - Clamped `initial_body = (total > headers_len) ? (total - headers_len) : 0` to `content_length`.
  - Sent both headers and available body together in a single contiguous `send_all(con->client_fd, buffer, headers_len + initial_body)`.
  - Extracted `stream_remaining()` helper to stream only the remaining bytes if `content_length > initial_body`.
  - For small API responses (< 4 KB), `stream_body` is completely bypassed.
- **Results**:
  - `local_benchmark.js`: **19,218.4 req/sec** (Duration: **1.04s**, down from 1.08s $\rightarrow$ **+4.2% speedup**).
  - Native C Reverse Proxy Stress (`-m stress` proxied): **23,078.26 req/sec** (Duration: **0.867s**, down from 0.979s $\rightarrow$ **+12.4% throughput jump**).
  - Syscall Reduction: Slashed response-path `send()` syscalls by **50%**.
  - All test suites passing 100% (Fallback circuit breaker, 5-slot rotation, real-world web matrix).

---

### Optimization #6: Single-Log Hot Path & Mutex Contention Reduction
- **Date**: 2026-11-04
- **Files Modified**: `src/proxy.c`
- **The Bottleneck**:
  Every request called `write_log(LOG_INFO, ...)` twice: once on receipt of client headers (`write_log(LOG_INFO, "%s %s %s")`) and once on completion (`write_log(LOG_INFO, "%s %s -> %d ... in %llu ms")`). Under high concurrency across 8 worker threads, all threads competed for the single `log_mutex` twice per request (40,000 mutex lock/unlock operations per 20k request burst).
- **The Solution**:
  Demoted the initial request-line log at `src/proxy.c:239` to `LOG_DEBUG`. The completion log at line 336 already captures full transaction telemetry (method, target host, status code, bytes, and latency).
- **Results**:
  - `local_benchmark.js` under `-l info`: Climbed from **14,075 req/s** to **16,494.5 req/s** (**+17.2% throughput improvement**).
  - Native C Reverse Proxy Stress (`-m stress` proxied): Surged from **23,078 req/s** to **28,638.70 req/sec** (**0.698s for 20,000 requests** $\rightarrow$ **+24.1% throughput jump**).
  - Sustained Heavy Load: **50,000 requests in 2.02 seconds** (**24,662.1 req/s**) with 0 failures across 150 concurrent connections.
  - Mutex Acquisitions: Slashed by **50%** in default info logging mode.
  - Zero debug data loss: Full request details remain available with `-v` / `-l debug`.

### Optimization #7: Silent-Path Clock Bypass & Reactor Fleet Scaling
- **Date**: 2026-11-04
- **Files Modified**: `include/logger.h`, `src/logger.c`, `src/proxy.c`
- **The Bottleneck**:
  Even when running in silent/error logging modes (`-l error`), `proxy_handle_client()` called `get_time_ms()` twice per transaction to calculate `latency_ms`. Across a 50,000-request burst, this generated **100,000 redundant `clock_gettime()` vDSO calls** and format-string overhead for logs that were never printed. Furthermore, single-process mock upstreams capped at ~25k req/s, bottlenecking proxy measurements.
- **The Solution**:
  - Exported `current_log_level` globally via `extern log_level_t current_log_level;` in `include/logger.h`.
  - Conditioned `get_time_ms()` and the transaction completion log on `if (current_log_level <= LOG_INFO)`, completely bypassing timer overhead when silent.
  - Benchmarked against multi-worker clustered mock upstream (`-t 16 -c 200`).
- **Results**:
  - **Native C Stress Engine**: Broke through the 50k barrier, reaching **52,643.87 req/sec** (60,000 requests in **1.140s**, 0 failures).
  - **Local Cluster Benchmark (`local_benchmark.js`)**: **17,741.2 req/sec** (1.13s for 20,000 requests).
  - All test suites (Rotation, Fallback Circuit Breaker) continue to pass 100%.

---

## 🚫 Experimental Post-Mortems & Negative Results (What NOT to Do)

In high-performance systems engineering, documenting negative results is just as vital as recording wins. The following approaches were empirically tested, identified as regressions, and intentionally discarded or reverted:

### Post-Mortem A: Removing `EPOLLONESHOT` in Linux Edge-Triggered (`EPOLLET`) Mode
- **Theory**: Sockets are private to their worker thread (via `SO_REUSEPORT`), so removing `EPOLLONESHOT` would avoid calling `epoll_ctl(EPOLL_CTL_MOD)` on every keep-alive request cycle.
- **Reality & Empirical Failure**: Under Linux edge-triggered mode (`EPOLLET`), removing `EPOLLONESHOT` resulted in silent socket stalls and edge-miss timeouts. If the client pipeline delivers bytes unevenly or if a read loop terminates before an explicit `EAGAIN` transition, `epoll_wait` never wakes up again for that descriptor.
- **Decision**: **REVERTED**. Re-arming with `EPOLLONESHOT` via `session->engine->mod(...)` in `session_reset()` is strictly mandatory for rock-solid Linux edge semantics.

### Post-Mortem B: Eager Header Pre-Parsing in `parse_request()`
- **Theory**: Slicing headers and eagerly storing `content_length`, `keep_alive`, and `target_url` during `parse_request()` would avoid linear traversals via `get_header_value()` in `proxy_handle_client()`.
- **Reality & Empirical Failure**: The second parsing loop in `parse_request()` added uniform CPU branch overhead to every incoming request. Because reverse proxy requests only query 2-3 specific headers anyway, the small inline linear search with immediate early-exit in `get_header_value()` is already cache-resident and measurably faster.
- **Decision**: **REVERTED**. Clean, simple, early-exit scans beat multi-pass eager pre-parsing.


