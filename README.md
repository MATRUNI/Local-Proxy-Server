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
  - [Automated 50,000+ Req/Sec Benchmark Suite](#0-automated-50000-reqsec-benchmark-suite-scriptsbenchmark_50ksh)
  - [Native C Multi-Threaded Stress Engine](#1-native-c-multi-threaded-stress-engine--m-stress)
  - [Master Webpage & CDN Test Suite](#2-master-webpage--cdn-test-suite-scriptstest_suitejs)
  - [High-Throughput Local Proxy Benchmark](#3-high-throughput-local-proxy-benchmark-scriptslocal_benchmarkjs)
  - [Carousel Log Rotation Audit](#4-carousel-log-rotation--active-marker-audit-scriptstest_rotationjs)
  - [Kernel Memory & Concurrency Benchmark](#5-kernel-memory--concurrency-benchmark-scriptstest_memorysh)
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
│   ├── stress.h        # Multi-threaded native C stress engine configuration & runner
│   ├── tls.h           # OpenSSL TLS client wrappers, context lifecycle, read/write
│   └── url.h           # URL parser (schema, host, port, path, query extraction)
├── src/
│   ├── main.c          # Mode dispatcher, Multi-Reactor event loop, worker fleet runner
│   ├── config.c        # CLI argument parser (-p, -b, -t, -m, -l, -v, -u, -c, -n, -X, -d)
│   ├── server.c        # BSD socket creation, non-blocking helpers, bind, listen
│   ├── proxy.c         # HTTP transaction pipeline, request forwarding, response streaming
│   ├── http.c          # In-place HTTP header slicing and protocol parsing
│   ├── http_error.c    # Error page HTML generation with CORS headers
│   ├── logger.c        # Asynchronous double-buffering logger thread implementation
│   ├── stress.c        # Native C multi-threaded non-blocking epoll stress engine
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

The codebase is engineered to be **100% cross-platform**, multiplexing connections using the optimal kernel event engine for each operating system:
- **Linux**: Multi-Reactor `epoll` (`EPOLLET` + `SO_REUSEPORT`)
- **macOS / BSD**: Native `kqueue` (`EVFILT_READ` / `EVFILT_WRITE`)
- **Windows**: `WSAPoll` (`Winsock2` non-blocking sockets)

---

### 🐧 Linux (Ubuntu / Debian / Fedora / Arch)

#### Prerequisites
- Compiler: `gcc` or `clang` (C99/GNU99)
- Libraries: OpenSSL (`libssl-dev`), POSIX Threads (`-pthread`)

```bash
# Ubuntu / Debian
sudo apt update && sudo apt install -y build-essential libssl-dev

# Fedora / RHEL
sudo dnf install -y gcc openssl-devel

# Arch Linux
sudo pacman -S base-devel openssl
```

#### Compilation
```bash
gcc -O3 -flto -march=native -Wall -Wextra -Werror -Iinclude src/*.c -pthread -lssl -lcrypto -o build/api
```

---

### 🍏 macOS (Apple Silicon M1/M2/M3/M4 & Intel x86_64)

On macOS, the proxy automatically selects **native BSD `kqueue`** (`src/poll_kqueue.c`). Because Apple does not ship OpenSSL in default compiler search paths, use Homebrew to install OpenSSL.

#### Prerequisites
```bash
# 1. Install Xcode Command Line Tools (if not already installed)
xcode-select --install

# 2. Install OpenSSL via Homebrew
brew install openssl@3
```

#### Compilation (Apple Clang / GCC)
Point the compiler to Homebrew's OpenSSL headers and library paths using `$(brew --prefix openssl)`:

```bash
# Compile for macOS (auto-links native kqueue)
clang -O3 -flto -Wall -Wextra -Werror \
  -Iinclude \
  -I$(brew --prefix openssl)/include \
  -L$(brew --prefix openssl)/lib \
  -pthread -lssl -lcrypto \
  src/*.c -o build/api
```

*(Note: On Apple Silicon, Clang automatically targets ARM64; `-march=native` is omitted for seamless Apple Silicon & Intel Universal binary compilation).*

---

### 🪟 Windows (MSYS2 / MinGW-w64)

On Windows, the proxy automatically selects **`WSAPoll`** (`src/poll_wsapoll.c`) and links against `ws2_32`.

#### Prerequisites (MSYS2 MinGW 64-bit terminal)
```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-openssl
```

#### Compilation
```bash
gcc -O3 -flto -Wall -Wextra -Werror -Iinclude src/*.c -pthread -lssl -lcrypto -lws2_32 -o build/api.exe
```

---

### 📦 Minimal Size Binary (Embedded / IoT Mode)
For ultra-compact deployments (Raspberry Pi, OpenWrt, microVMs), compile with dead-code elimination and symbol stripping to generate a **47 KB standalone binary**:

```bash
gcc -Os -flto -march=native -ffunction-sections -fdata-sections -Wl,--gc-sections -s -Wall -Wextra -Werror -Iinclude src/*.c -pthread -lssl -lcrypto -o build/api
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
       ./api -m stress -u <url> -c <concurrency> -n <requests> -t <threads> -X <method> -d <data>
```

| Flag | Long Option | Description | Default |
| :--- | :--- | :--- | :--- |
| `-p` | | Port to listen on (`1-65535`) | `17777` |
| `-b` | `--bind` | IP address to bind to | `127.0.0.1` |
| `-t` | `--threads` | Number of worker threads (clamped to hardware limits) | Auto (`sysconf`) |
| `-m` | `--mode` | Run mode (`server`, `logs`, `help`, `stress`) | `server` |
| `-l` | `--log-level` | Log verbosity (`debug`, `info`, `warn`, `error` or `d, i, w, e`) | `info` |
| `-v` | `--verbose` | Shortcut for `-l debug` | `false` |
| `-u` | `--url` | Target URL to benchmark (`-m stress`) | `http://127.0.0.1:17777/` |
| `-c` | `--concurrent` | Concurrency: number of parallel connections | `50` |
| `-n` | `--requests` | Total requests to execute (`-m stress`) | `10000` |
| `-X` | `--method` | Custom HTTP method (`GET`, `POST`, `PUT`, `DELETE`) | `GET` |
| `-d` | `--data` | HTTP payload / body for requests | *(empty)* |
| `-h` | `--help` | Display usage manual | |

### Examples
```bash
# 1. Start proxy on port 8080 with 8 worker threads
./build/api -p 8080 -b 0.0.0.0 -t 8 -v

# 2. Native C Stress Benchmark: 20,000 requests, 100 concurrent connections, 4 threads
./build/api -m stress -u http://127.0.0.1:17777/?url=http://127.0.0.1:9099/test -c 100 -n 20000 -t 4

# 3. High-throughput custom POST benchmark with JSON payload
./build/api -m stress -u http://127.0.0.1:9099/test -c 100 -n 10000 -X POST -d '{"speed":"c"}'

# 4. View formatted color-coded runtime access logs
./build/api -m logs

# 5. Display help
./build/api -h
```

---

## 🧪 Testing & Benchmarks

The proxy includes a comprehensive testing and benchmarking toolkit:

### 0. Automated 50,000+ Req/Sec Benchmark Suite (`scripts/benchmark_50k.sh`)
An all-in-one automated benchmark script that tests the ultimate throughput limits of the proxy. It launches a 4-worker clustered mock upstream server, spins up the 16-thread C Reverse Proxy, primes socket tables, and executes a 60,000-request native C stress test with automatic cleanup:

```bash
./scripts/benchmark_50k.sh
```

#### Peak Measured Results (60,000 Requests, 200 Concurrency, 8 Threads)
```text
=======================================================
                  STRESS TEST RESULTS                  
=======================================================
  Target:             http://127.0.0.1:17777/?url=http://127.0.0.1:9099/
  Method:             GET
  Concurrency:        200 connections
  Threads:            8 worker threads
-------------------------------------------------------
  Total Requests:     60000
  Completed:          60002
  Failed:             0
  Duration:           1.187 seconds
-------------------------------------------------------
  🔥 Throughput:      50,565.15 req/sec
=======================================================
```

---

### 1. Native C Multi-Threaded Stress Engine (`-m stress`)
A built-in, zero-dependency, multi-threaded C benchmarking engine powered by non-blocking event loops (`epoll` on Linux, `kqueue` on macOS, `WSAPoll` on Windows). It eliminates Node.js client CPU bottlenecks and supports persistent HTTP/1.1 keep-alive, custom HTTP methods, custom payloads, and TLS/SNI.

```
                     [ CLI Invocations / Config: run_stress_engine() ]
                       (Target URL, Concurrency: -c, Total Reqs: -n)
                                            │
                             [ Lock-Free Workload Divider ]
                      (conns = c / threads, reqs = n / threads)
                                            │
              ┌─────────────────────────────┼─────────────────────────────┐
              ▼                             ▼                             ▼
      [ Stress Worker #0 ]          [ Stress Worker #1 ]          [ Stress Worker #N-1 ]
     Isolated poll_engine          Isolated poll_engine          Isolated poll_engine
              │                             │                             │
         epoll_wait()                  epoll_wait()                  epoll_wait()
              │                             │                             │
      ┌───────┴───────┐             ┌───────┴───────┐             ┌───────┴───────┐
      ▼               ▼             ▼               ▼             ▼               ▼
  conn_0..conn_K  Keep-Alive    conn_0..conn_K  Keep-Alive    conn_0..conn_K  Keep-Alive
  (Non-blocking) (Pipelines)    (Non-blocking) (Pipelines)    (Non-blocking) (Pipelines)
      │               │             │               │             │               │
      └───────┬───────┘             └───────┬───────┘             └───────┬───────┘
              │                             │                             │
              ▼                             ▼                             ▼
    [ Finite State Machine ]      [ Finite State Machine ]      [ Finite State Machine ]
    CONN_CONNECTING (TCP/TLS)     CONN_CONNECTING (TCP/TLS)     CONN_CONNECTING (TCP/TLS)
              │                             │                             │
    CONN_SENDING (Outbound Req)   CONN_SENDING (Outbound Req)   CONN_SENDING (Outbound Req)
              │                             │                             │
    CONN_RECEIVING (\r\n\r\n)     CONN_RECEIVING (\r\n\r\n)     CONN_RECEIVING (\r\n\r\n)
              │                             │                             │
              └─────────────────────────────┼─────────────────────────────┘
                                            │
                                [ Main Thread Aggregator ]
                               pthread_join() + Stop Clock
                             (Total Req/s, Duration, Summary)
```

```bash
# 1. Direct upstream benchmark: 20,000 requests, 100 connections, 4 threads
./build/api -m stress -u http://127.0.0.1:9099/test -c 100 -n 20000 -t 4

# 2. Full reverse proxy benchmark (Client -> Proxy -> Upstream -> Client)
./build/api -m stress -u "http://127.0.0.1:17777/?url=http://127.0.0.1:9099/test" -c 100 -n 20000 -t 4

# 3. Custom payload POST benchmark
./build/api -m stress -u http://127.0.0.1:9099/test -c 100 -n 10000 -t 4 -X POST -d '{"speed":"c"}'
```

#### Benchmark Results (20,000 Requests, 100 Connections, 4 Threads)
```text
=======================================================
                  STRESS TEST RESULTS                  
=======================================================
  Target:             http://127.0.0.1:9099/test
  Method:             GET
  Concurrency:        100 connections
  Threads:            4 worker threads
-------------------------------------------------------
  Total Requests:     20000
  Completed:          20000
  Failed:             0
  Duration:           0.740 seconds
-------------------------------------------------------
  Throughput:         27,028.54 req/sec
=======================================================
```

### 2. Master Webpage & CDN Test Suite (`scripts/test_suite.js`)
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

### 5. Kernel Memory & Concurrency Benchmark (`scripts/test_memory.sh`)
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
9. **The Native C Stress Engine (27,000+ Req/Sec Benchmark Engine)**:
   - **Eliminating Node.js Overhead**: Removed external Node.js benchmark dependencies whose multi-process event loop saturated client CPU cores at 18k req/sec.
   - **Zero-Dependency Multi-Threaded Event Loop**: Built a pure C non-blocking stress client in `src/stress.c` reusing the core cross-platform abstraction `poll_engine` (`epoll`/`kqueue`/`WSAPoll`).
   - **Lock-Free Concurrency**: Partitioned connections and target request quotas evenly across worker threads with isolated event loops, avoiding all thread locking and mutex contention.
   - **Full Protocol Flexibility**: Added CLI flags (`-u`, `-c`, `-n`, `-X`, `-d`) for benchmarking custom HTTP methods (`POST`, `PUT`), custom JSON payloads, and dynamic TLS/SNI negotiation for HTTPS targets.
   - **Performance Milestone**: Reached **27,028 req/sec direct** and **25,127 req/sec fully proxied** with a 100% success rate across 20,000 requests.

---

## 📄 License
Open source under the MIT License.