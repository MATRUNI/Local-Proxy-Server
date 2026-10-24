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

The proxy uses the **Multi-Reactor Pattern** (similar to modern web servers like NGINX, Envoy, and HAProxy) to eliminate thread churn and context switching.

```
                              [ Incoming Client Connections ]
                                             │
                                             ▼
                                   [ Acceptor Thread (Boss) ]
                                  (server_accept on listen_fd)
                                             │
                             1. Sets socket to O_NONBLOCK
                             2. Round-Robin dispatch (fd % N)
                             3. Direct epoll_ctl(EPOLL_CTL_ADD)
                                             │
               ┌─────────────────────────────┼─────────────────────────────┐
               ▼                             ▼                             ▼
       [ Worker Thread #0 ]          [ Worker Thread #1 ]          [ Worker Thread #N-1 ]
       (Private epoll_fd)            (Private epoll_fd)            (Private epoll_fd)
               │                             │                             │
          epoll_wait()                  epoll_wait()                  epoll_wait()
               │                             │                             │
    ┌──────────┴──────────┐       ┌──────────┴──────────┐       ┌──────────┴──────────┐
    ▼                     ▼       ▼                     ▼       ▼                     ▼
Plain HTTP            OpenSSL  Plain HTTP            OpenSSL  Plain HTTP            OpenSSL
Socket (80)           TLS(443) Socket (80)           TLS(443) Socket (80)           TLS(443)
```

### 1. The Acceptor (Boss)
- Runs in `run_server()` on the main thread.
- Listens on `listen_fd` configured with `SO_REUSEADDR` and `SOMAXCONN` backlog.
- Welcomes incoming clients, marks the client socket as non-blocking (`O_NONBLOCK` via `fcntl`), and assigns the connection to a worker thread using round-robin distribution (`client_fd % config->threads`).
- Registers the socket directly into that worker's private Epoll tree using `epoll_ctl(..., EPOLL_CTL_ADD, ...)`.

### 2. The Worker Fleet (Waiters)
- Spawns a fixed fleet of permanent worker threads at startup (auto-scaled to match physical/logical CPU cores).
- Each worker owns an independent `epoll_fd` (Edge-Triggered `EPOLLIN | EPOLLET`).
- Workers sleep in kernel space (`epoll_wait`) at 0% CPU until the kernel signals that their assigned sockets have data.
- **Zero Lock Contention**: Workers don't compete over a single shared epoll queue, avoiding the "Thundering Herd" problem.

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
│   ├── runMain.sh          # Build and run script
│   ├── stop.sh             # Graceful shutdown helper
│   ├── test_memory.sh      # Kernel memory (VmRSS, VmHWM) and thread benchmark tool
│   ├── test_suite.js       # Master Node.js test suite for real webpages and concurrency bursts
│   └── local_benchmark.js  # Zero-latency local throughput benchmark (2,400+ req/s)
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

### 3. Local Zero-Latency Proxy Benchmark (`scripts/local_benchmark.js`)
Measures maximum raw proxying throughput, socket turnaround speed, and event-loop efficiency without external internet/DNS latency:
```bash
# In one terminal:
./build/api -p 17777 -l error

# In another terminal:
node scripts/local_benchmark.js
```

#### Benchmark Results (5,000 Requests, 50 Parallel Connections)
```text
==========================================================
       LOCAL ZERO-LATENCY PROXY BENCHMARK
==========================================================
Target Proxy   : http://127.0.0.1:17777
Mock Upstream  : http://127.0.0.1:9099
Total Requests : 5000
Concurrency    : 50 parallel clients

--- ⏱️ THROUGHPUT & PERFORMANCE ---
Completed Requests : 5000 / 5000 (100.0% success)
Total Time         : 1.61 seconds
Throughput         : 3113.5 requests / sec
Latency (Min)      : 5.07 ms
Latency (Avg)      : 15.97 ms
Latency (p95)      : 24.37 ms
Latency (Max)      : 49.35 ms
==========================================================
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

---

## 📄 License
Open source under the MIT License.