# Local Proxy Server (C) — High-Performance C10K Reverse Proxy

A lightweight, blazing-fast, and memory-efficient HTTP/HTTPS reverse proxy built from scratch in pure C. Designed to tackle the **C10K problem** (10,000 concurrent connections) using a **Multi-Reactor Epoll architecture**, an **asynchronous double-buffering logger**, and raw POSIX socket/TLS multiplexing with **zero external HTTP dependencies**.

---

## 📑 Table of Contents
- [Architecture Overview](#-architecture-overview)
- [Key Features & Engineering Highlights](#-key-features--engineering-highlights)
- [Project Structure](#-project-structure)
- [Prerequisites & Build](#-prerequisites--build)
- [CLI Usage & Flags](#-cli-usage--flags)
- [Testing & Benchmarks](#-testing--benchmarks)
- [The Journey & Engineering Evolution](#-the-journey--engineering-evolution)

---

## 🏛 Architecture Overview

The proxy uses the **Multi-Reactor Pattern with `SO_REUSEPORT`** (similar to modern ultra-high performance servers like NGINX and Envoy) to eliminate thread churn, context switching, and single-thread accept bottlenecks.

```
                              [ Incoming Client Connections ]
                                             │
                       [ Linux Kernel SO_REUSEPORT Load Balancer ]
                        (Hardware/Kernel-space 4-tuple IP hashing)
                                             │
               ┌─────────────────────────────┼─────────────────────────────┐
               ▼                             ▼                             ▼
       [ Worker Thread #0 ]          [ Worker Thread #1 ]          [ Worker Thread #N-1 ]
      listen_fd + epoll_fd          listen_fd + epoll_fd          listen_fd + epoll_fd
               │                             │                             │
          epoll_wait()                  epoll_wait()                  epoll_wait()
               │                             │                             │
       ┌───────┴───────┐             ┌───────┴───────┐             ┌───────┴───────┐
       ▼               ▼             ▼               ▼             ▼               ▼
   accept()      session_handle   accept()      session_handle   accept()      session_handle
(New Clients)   (Keep-Alive Req) (New Clients)  (Keep-Alive Req) (New Clients)   (Keep-Alive Req)
       │               │             │               │             │               │
       ▼               ▼             ▼               ▼             ▼               ▼
  Plain HTTP /    OpenSSL TLS   Plain HTTP /    OpenSSL TLS   Plain HTTP /    OpenSSL TLS
   Stream (80)     Client(443)   Stream (80)     Client(443)   Stream (80)     Client(443)
```

### 1. `SO_REUSEPORT` Parallel Worker Fleet
- Every worker thread creates and owns its **own private listening socket** bound to port 17777 using `SO_REUSEPORT`.
- **Zero Accept Bottleneck**: The Linux kernel distributes incoming connections across all worker threads in kernel space directly at the socket layer.
- Workers sleep in kernel space (`epoll_wait`) at 0% CPU until the kernel signals new client connections or incoming HTTP data.
- **Zero Lock Contention**: Workers accept, process, stream, and log connections independently with complete thread affinity.
- The `main` thread simply supervises worker lifecycles and handles clean signal shutdown (`SIGINT`, `SIGTERM`).

### 2. HTTP Keep-Alive & Upstream Connection Reuse
- **Downstream Keep-Alive**: When a client requests persistent connections, the proxy keeps `client_fd` open, resets the request parser with `session_reset()`, and re-arms the socket in `epoll` (`EPOLL_CTL_MOD`). One client connection can stream thousands of requests without repeat handshakes.
- **Upstream Connection Reuse**: Remembers the connected `upstream_host` and `upstream_port`. Repeated requests to the same target skip `make_api_call()` and `connect()`, streaming HTTP requests immediately over the existing pipe.
- **`TCP_NODELAY` (Nagle Bypass)**: Disables Nagle's buffering algorithm on all client and upstream sockets. Eliminates the notorious **40ms Linux Delayed-ACK penalty**, dropping sequential request latency from **41.9ms to 0.46ms (91x faster)**.
- **IP Fast-Path**: Uses `inet_pton()` to detect numeric IPv4 destinations immediately, bypassing glibc `getaddrinfo()` resolver lookups and saving ~0.3ms per request.

### 3. Asynchronous Double-Buffering Logger ("Two Buckets Under a Tap")
- Worker threads **never perform blocking disk I/O**.
- Logging uses two 64 KB RAM buckets (`BUCKET_A` and `BUCKET_B`):
  - **The Tap (Producers)**: Worker threads format messages into stack buffers and copy them into `active_buffer` with a ~20-nanosecond `memcpy`.
  - **The Janitor (Consumer)**: A dedicated background logging thread waits on a condition variable (`pthread_cond_wait`). When the active bucket fills up or the doorbell is signaled, it atomically swaps the two pointers (`char *temp = active; active = waiting; waiting = temp;`), unlocks the mutex immediately, and commits the full chunk to disk with `fwrite()` + `fflush()`.
- Disk latency never stalls client HTTP proxying.

### 4. Self-Healing 5-Slot Carousel Log Rotation
- **Zero Log Bloat**: Prevents runaway log files by bounding logging to a fixed 5-slot ring buffer (`logger_0.log` through `logger_4.log`) capped at 2 MB per file.
- **In-Band Active Marker (`@`)**: The currently active file is dynamically prefixed with `@` (e.g. `@logger_0.log`). When a slot reaches `MAX_FILE_SIZE`:
  1. It drops the `@` prefix via `rename()`.
  2. Advances slot index via boundary check `(current_slot + 1 >= FILE_COUNT) ? 0 : current_slot + 1`.
  3. Renames the target file to `@`, truncates previous contents with `fopen(..., "w")`, and opens the append stream.
- **Crash Recovery & Conflict Resolution**: On startup, `last_active_file()` scans `logs/` via `opendir()` and `readdir()`. If a prior crash or interrupted rotation left multiple `@` files, it calculates the true active slot across progression and wrap-around boundaries (`4 -> 0`), auto-heals by stripping the `@` from stale duplicates, and resumes appending with zero state loss.

---

## ✨ Key Features & Engineering Highlights

- **Hardware-Adaptive Auto-Configuration**: Queries the Linux kernel with `sysconf(_SC_NPROCESSORS_ONLN)` to detect available CPU cores and spawns the optimal thread count automatically. Includes protection against user thread overload (`-t`) with automatic clamping.
- **Dual HTTP & HTTPS Engine**:
  - Automatically negotiates OpenSSL TLS 1.3 with upstream servers for `https://` URLs using Server Name Indication (SNI).
  - Handles plain HTTP port 80 over standard stream sockets.
- **RFC 7230 Compliant Header Rewriting**:
  - Automatically reconstructs clean upstream request lines.
  - Strips conflicting duplicate `Host` headers to prevent 400 Bad Request rejections on strict CDN frontends (e.g. GitHub, Cloudflare).
- **Chunked Transfer-Encoding Termination**:
  - Inspects incoming response bodies using binary memory comparisons (`memcmp`) to catch chunked termination sequences (`0\r\n\r\n`) across compressed gzip/binary streams without stalling on keep-alive socket timeouts.
- **Bounded Carousel Log Engine**:
  - High-performance asynchronous double-buffered ring buffer with 5 rotating slots.
  - Zero external state files—uses in-band `@` marker with automatic collision resolution and startup healing.
- **CORS Support**:
  - Handles preflight `OPTIONS` requests natively, returning `204 No Content` with appropriate `Access-Control-Allow-*` headers.
- **Graceful Lifecycle Management**:
  - Signal handling (`SIGINT`, `SIGTERM`) registered via `sigaction` without `SA_RESTART`, allowing blocking system calls (`accept`, `epoll_wait`) to interrupt cleanly.
  - Automatically joins all worker threads, flushes logging buffers, and frees all memory structures on shutdown with zero leaks.

---

## 📁 Project Structure

```text
.
├── include/
│   ├── config.h        # Project constants (stack sizes, timeouts, proxy_config struct)
│   ├── http.h          # HTTP request/response structs, header parsers, status codes
│   ├── http_error.h    # RFC error response generators (400, 404, 405, 413, 500, 502)
│   ├── logger.h        # Asynchronous logger API and log levels (DEBUG, INFO, WARN, ERROR)
│   ├── proxy.h         # Proxy connection management, streaming, and upstream dispatch
│   ├── server.h        # Socket lifecycle, timeout configuration, and accept handlers
│   ├── tls.h           # OpenSSL TLS client wrappers, context lifecycle, read/write
│   └── url.h           # URL parser (schema, host, port, path, query extraction)
├── src/
│   ├── main.c          # Mode dispatcher, Multi-Reactor event loop, worker fleet runner
│   ├── config.c        # CLI argument parser (-p, -b, -t, -m, -l, -v) and validation
│   ├── server.c        # BSD socket creation, non-blocking helpers, bind, listen
│   ├── proxy.c         # HTTP transaction pipeline, request forwarding, response streaming
│   ├── http.c          # In-place HTTP header slicing and protocol parsing
│   ├── http_error.c    # Error page HTML generation with CORS headers
│   ├── logger.c        # Asynchronous double-buffering logger thread implementation
│   ├── tls.c           # TLS handshake negotiation, SSL context, and socket wrappers
│   └── url.c           # Protocol and hostname URL parsing
├── scripts/
│   ├── local_benchmark.js  # High-throughput benchmark (supports keep-alive & clustering, 17k+ req/s)
│   ├── test_rotation.js    # 5-slot carousel log rotation and active marker audit
│   ├── test_fallback.js    # Upstream failover, DNS fallback, and error recovery test
│   ├── test_suite.js       # Master Node.js test suite for real webpages and CDNs
│   ├── test_memory.sh      # Kernel memory (VmRSS, VmHWM) and thread benchmark tool
│   ├── runMain.sh          # Build and run script
│   └── stop.sh             # Graceful shutdown helper
├── logs/               # Rotating carousel logs (@logger_X.log, logger_X.log, test reports)
├── build/              # Compiled binary outputs
├── TODO.md             # Development roadmap and historical notes
└── README.md           # Project documentation
```

---

## 🛠 Prerequisites & Build

### Prerequisites
- **OS**: Linux (tested on Ubuntu / Debian x86_64)
- **Compiler**: `gcc` (with C99 or GNU99 support)
- **Libraries**: OpenSSL development headers (`libssl-dev`), POSIX Threads (`-pthread`)
- **Testing**: `node` (v18+ for test suite) and `curl`

Install dependencies on Ubuntu/Debian:
```bash
sudo apt update
sudo apt install -y build-essential libssl-dev
```

### Compilation
Compile the project with full optimization and warnings enabled:

```bash
gcc -Wall -Wextra -Iinclude src/*.c -pthread -lssl -lcrypto -o build/api
```

---

## 🚀 CLI Usage & Flags

### Start the Server (Defaults)
```bash
./build/api
```
*Defaults to port `17777`, listening on `127.0.0.1`, with worker threads auto-detected from online CPU cores.*

### Command-Line Options
```text
Usage: ./api -p <port> -b <ip> -t <threads> -m <server|stress|logs|help> -l <debug|info|warn|error> -v
```

| Flag | Long Option | Description | Default |
| :--- | :--- | :--- | :--- |
| `-p` | | Port to listen on (`1-65535`) | `17777` |
| `-b` | `--bind` | IP address to bind to | `127.0.0.1` |
| `-t` | `--threads` | Number of worker threads (clamped to hardware limits) | Auto (`sysconf`) |
| `-m` | `--mode` | Run mode (`server`, `logs`, `help`, `stress`) | `server` |
| `-l` | `--log-level` | Log verbosity (`debug`, `info`, `warn`, `error` or `d, i, w, e`) | `info` |
| `-v` | `--verbose` | Shortcut for `-l debug` | `false` |
| `-h` | `--help` | Display usage manual | |

### Examples
```bash
# Start on port 8080 bound to all interfaces with 8 threads in verbose mode
./build/api -p 8080 -b 0.0.0.0 -t 8 -v

# View formatted color-coded runtime access logs
./build/api -m logs

# Display help
./build/api -h
```

---

## 🧪 Testing & Benchmarks

The proxy includes two automated test and benchmark tools:

### 1. Master Webpage & CDN Test Suite (`scripts/test_suite.js`)
Runs diverse real-world tests against live production targets, verifying HTTP status codes, chunked streaming, and CORS headers:
- **Google Homepage**: Full HTML, cookies (`Set-Cookie`), gzip compression (~90 KB)
- **Wikipedia**: Heavyweight article on the C Programming Language (~735 KB)
- **Hacker News**: Cloudflare-protected dynamic frontpage HTML
- **DuckDuckGo**: Semantic search engine HTML
- **Plain HTTP Port 80**: Non-TLS stream verification on `example.com`
- **POST Streaming**: JSON body forwarding to `httpbin.org/post`
- **Error Handling**: Validates `400 Bad Request`, `404 Not Found`, `500 Server Error`, and `502 Bad Gateway` (DNS failure)
- **Parallel Burst**: Fires 50 simultaneous requests across CDNs to measure concurrency latency.

Run the test suite:
```bash
# 1. Start proxy in one terminal
./build/api -p 17777

# 2. Run master test suite in another terminal
node scripts/test_suite.js
```

### 2. Kernel Memory & Concurrency Benchmark (`scripts/test_memory.sh`)
Measures kernel memory metrics (`VmRSS`, `VmHWM`) under heavy parallel load streaming 100 KB HTTPS files from remote servers:
```bash
bash scripts/test_memory.sh
```

#### Benchmark Results (8 Cores, 30 Concurrent Clients, 60 HTTPS Requests)
```text
==========================================================
          STRESS & MEMORY BENCHMARK REPORT                
==========================================================
Total Requests:       60 (60 success, 0 failed)
Peak Active Threads:  10 (1 listener + 1 logger + 8 epoll workers)

--- ⏱️ CONCURRENCY TIME & THROUGHPUT ---
Burst Duration:       31.92 seconds
Throughput:           1.9 req/sec
Latency (Min/Avg/Max): 2149 ms / 8224 ms / 26745 ms

--- 🧠 KERNEL RAM FOOTPRINT (VmRSS & VmHWM) ---
Baseline VmRSS (Idle):        8920 kB (~8.7 MB)
Sampled Peak VmRSS:           11880 kB (~11.6 MB)
Kernel VmHWM (High Water):    11880 kB (~11.6 MB)
----------------------------------------------------------
Actual RAM Added By Stress:   2960 kB (~2.8 MB)
RAM Footprint Per Connection: 98.7 kB / client
Cooldown VmRSS (After Load):  11880 kB
==========================================================
```

### 3. High-Throughput Local Proxy Benchmark (`scripts/local_benchmark.js`)
Tests raw proxying throughput, socket turnaround speed, and event-loop efficiency without external internet/DNS latency. Supports multi-core clustering and persistent HTTP Keep-Alive:

```bash
# In one terminal:
./build/api -p 17777 -l error

# In another terminal:
# 1. Multi-Process Cluster Mode (Default: 4 client workers, 100 connections)
node scripts/local_benchmark.js

# 2. Single-Process Keep-Alive Mode
node scripts/local_benchmark.js --single

# 3. Short-Lived Connections Mode (Connection: close)
node scripts/local_benchmark.js --no-keepalive
```

#### Benchmark Results (20,000 Requests, 4 Client Processes, 100 Persistent Connections)
```text
===============================================================
        LOCAL HIGH-THROUGHPUT PROXY BENCHMARK                  
===============================================================
Target Proxy   : http://127.0.0.1:17777
Mock Upstream  : http://127.0.0.1:9099
Total Requests : 20,000
Concurrency    : 100 connections (Keep-Alive)
Mode           : Multi-Process Cluster (4 client workers)

---------------- RESULTS ----------------
Successful Requests : 20000 / 20000 (100.0%)
Failed Requests     : 0
Total Time          : 1.16 seconds
🔥 THROUGHPUT       : 17,305.2 req/sec
===============================================================
```

#### ⚡ Latency Impact of `TCP_NODELAY` (The 40ms Nagle Fix)
By disabling Nagle's algorithm and preventing collision with Linux's delayed ACK timer (`TCP_DELACK_MIN`), request latency was slashed by **91x**:

| Configuration | Sequential Req Latency | Max Throughput | Notes |
| :--- | :--- | :--- | :--- |
| **Without `TCP_NODELAY`** | ~41.90 ms | ~24 req/s (sequential) | Linux waits 40ms before sending ACK |
| **With `TCP_NODELAY`** | **0.46 ms** | **17,305.2 req/s** | Immediate packet flush without stall |

---

### 4. Carousel Log Rotation & Active Marker Audit (`scripts/test_rotation.js`)
Validates live rotation under heavy throughput, ensuring the 5-slot carousel rotates at exactly 2 MB (`MAX_FILE_SIZE = 2097152`), migrates the in-band `@` marker, and preserves historical logs:

```bash
node scripts/test_rotation.js
```

```text
===============================================================
       LIVE ROTATION & COMMIT-BEFORE-RETIRE TEST SUITE        
===============================================================
Initial Active Log File : @logger_0.log
Active Log File to Rotate: @logger_0.log
Firing parallel requests to cross 2 MB threshold (~12,000 requests)...
Sent: 250 reqs  | Active: @logger_0.log (0.04 MB / 2.00 MB)
...
Sent: 11750 reqs | Active: @logger_0.log (1.98 MB / 2.00 MB)
Sent: 12000 reqs | Active: @logger_1.log (0.03 MB / 2.00 MB)
🎉 ROTATION DETECTED! Active marker moved to: @logger_1.log
===============================================================
                       FINAL LOGS AUDIT                        
===============================================================
  - @logger_1.log        : 26.5 KB   ⭐ ACTIVE
  - logger_0.log         : 2048.0 KB 📦 ARCHIVED
---------------------------------------------------------------
Active markers found   : 1 (Expected: exactly 1)
Total Requests Sent    : 12,000
Elapsed Time           : 4.47s
✅ PASS: Carousel rotation, commit-before-retire & active marker verified!
===============================================================
```

---

## 📖 The Journey & Engineering Evolution

This project was built from absolute scratch as a deep dive into systems programming, computer networking, and memory architectures:

1. **Low-Level BSD Sockets**: Understanding `socket()`, `setsockopt()`, `bind()`, `listen()`, `accept()`, and handling kernel interrupts like `SIGPIPE` (`signal(SIGPIPE, SIG_IGN)`).
2. **In-Place HTTP Parsing**: Parsing headers with raw pointer arithmetic, slicing request boundaries, and managing dynamic header arrays with `realloc`.
3. **OpenSSL TLS 1.3 Integration**: Managing `SSL_CTX`, wrapping socket descriptors with `SSL_set_fd()`, and learning the relationship between `void *` abstractions and cryptographic handshakes.
4. **The 256 KB Stack Discovery**: Diagnosing stack overflows during OpenSSL TLS handshakes and discovering why crypto threads require at least 256 KB stack allocations, while I/O threads can run on 32 KB.
5. **Asynchronous Double-Buffering Logger**: Inventing the "two buckets under a tap" model, coordinating thread synchronization with `pthread_mutex_t` and `pthread_cond_t`, and avoiding disk I/O bottlenecks.
6. **The Multi-Reactor Epoll Fleet**: Evolving from thread-per-connection to a fixed, core-pinned Epoll worker fleet, solving connection distribution, graceful termination timeouts, and binary chunked stream parsing (`memcmp`).
7. **The Self-Healing 5-Slot Carousel Logger**: Solving unbounded log growth with a fixed-ring carousel buffer, designing in-band `@` active marker tags without external metadata files, and building crash-resilient startup recovery using `opendir()`, `readdir()`, and wrap-around ring arithmetic.
8. **The 17,000+ Req/Sec Keep-Alive & `SO_REUSEPORT` Breakthrough**:
   - **`SO_REUSEPORT` Fleet**: Replaced the single-acceptor bottleneck with kernel-level connection balancing across independent worker listening sockets.
   - **Bidirectional Keep-Alive & Upstream Cache**: Eliminated double-handshake overhead per request by keeping client sockets persistent (`session_reset()`) and caching open upstream TCP connections.
   - **40ms Nagle / Delayed-ACK Discovery**: Diagnosed the 40ms latency freeze caused by Nagle's algorithm waiting on Linux's delayed ACK timer, resolving it with `TCP_NODELAY` and dropping latency from 41.9ms to 0.46ms (91x speedup).
   - **DNS Resolver Bypass**: Bypassed glibc's `getaddrinfo()` NSS file-reads on IP literals using `inet_pton()`.
   - **Performance Milestone**: Scaled from 3,113 req/sec to **17,305.2 req/sec** with 100% success rate across 20,000 requests.

---

## 📄 License
Open source under the MIT License.