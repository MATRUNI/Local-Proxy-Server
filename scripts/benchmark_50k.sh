#!/usr/bin/env bash
set -e

# ==============================================================================
# scripts/benchmark_50k.sh
# 
# Automated 50,000+ Req/Sec End-to-End Reverse Proxy Benchmark
# - Starts a 4-worker clustered mock upstream server on port 9099
# - Starts the C Reverse Proxy on port 17777 (16 worker threads, error log mode)
# - Performs socket priming and JIT warm-up
# - Executes the high-concurrency peak stress run
# - Automatically cleans up all background processes on exit
# ==============================================================================

MOCK_PORT=9099
PROXY_PORT=17777
CONCURRENCY=${2:-200}
TOTAL_REQUESTS=${1:-60000}
STRESS_THREADS=8
PROXY_THREADS=16

MOCK_SCRIPT="/tmp/mock_cluster_upstream_${MOCK_PORT}.js"

# Terminal formatting
C_BOLD="$(tput bold 2>/dev/null || true)"
C_GREEN="$(tput setaf 2 2>/dev/null || true)"
C_CYAN="$(tput setaf 6 2>/dev/null || true)"
C_YELLOW="$(tput setaf 3 2>/dev/null || true)"
C_RED="$(tput setaf 1 2>/dev/null || true)"
C_RESET="$(tput sgr0 2>/dev/null || true)"

echo "${C_BOLD}${C_CYAN}"
echo "==============================================================="
echo "       50,000+ REQ/SEC NATIVE PROXY BENCHMARK SUITE            "
echo "===============================================================${C_RESET}"

# 1. Ensure binary is compiled
BIN="${SANKRAM_BIN:-build/sankram}"
if [ ! -f "$BIN" ]; then
    if [ -f "build/api" ]; then
        BIN="build/api"
    else
        echo "${C_YELLOW}[1/3] Binary not found. Compiling with -O3 -flto...${C_RESET}"
        gcc -O3 -flto -march=native -Wall -Wextra -Werror -Iinclude src/*.c -pthread -lssl -lcrypto -o build/sankram
        BIN="build/sankram"
    fi
fi
echo "${C_GREEN}[1/3] Found production binary: ./${BIN}${C_RESET}"

kill_port() {
    local p=$1
    if command -v fuser >/dev/null 2>&1; then
        fuser -k "${p}/tcp" 2>/dev/null || true
    elif command -v lsof >/dev/null 2>&1; then
        lsof -ti ":${p}" | xargs kill -9 2>/dev/null || true
    fi
}

# Ensure ports are clear
kill_port ${MOCK_PORT}
kill_port ${PROXY_PORT}
sleep 0.2

# Cleanup handler
cleanup() {
    echo -e "\n${C_YELLOW}Cleaning up benchmark processes...${C_RESET}"
    if [ -n "$PROXY_PID" ]; then
        kill -SIGINT "$PROXY_PID" 2>/dev/null || true
        wait "$PROXY_PID" 2>/dev/null || true
    fi
    if [ -n "$MOCK_PID" ]; then
        kill -SIGTERM "$MOCK_PID" 2>/dev/null || true
        pkill -P "$MOCK_PID" 2>/dev/null || true
    fi
    rm -f "$MOCK_SCRIPT"
    kill_port ${MOCK_PORT}
    kill_port ${PROXY_PORT}
}
trap cleanup EXIT INT TERM

# 2. Start Clustered Mock Upstream (4 workers)
echo "${C_GREEN}[2/3] Starting 4-worker clustered upstream on port ${MOCK_PORT}...${C_RESET}"
cat << 'EOF' > "$MOCK_SCRIPT"
const cluster = require('cluster');
const http = require('http');

const port = parseInt(process.env.MOCK_PORT || '9099', 10);

if (cluster.isPrimary) {
  for (let i = 0; i < 4; i++) {
    cluster.fork();
  }
} else {
  http.createServer((req, res) => {
    res.writeHead(200, {
      'Content-Type': 'application/json',
      'Content-Length': '27',
      'Connection': 'keep-alive'
    });
    res.end('{"status":"ok","speed":"c"}');
  }).listen(port, '127.0.0.1');
}
EOF

MOCK_PORT=$MOCK_PORT node "$MOCK_SCRIPT" &
MOCK_PID=$!

# 3. Start C Proxy Server (16 threads, silent log mode)
echo "${C_GREEN}[3/3] Starting Sankram Proxy on port ${PROXY_PORT} (-t ${PROXY_THREADS} -l error)...${C_RESET}"
./$BIN -p ${PROXY_PORT} -l error -t ${PROXY_THREADS} > logs/benchmark_proxy.log 2>&1 &
PROXY_PID=$!

# Wait for both services to be ready
READY=0
for i in {1..30}; do
    if curl -s "http://127.0.0.1:${PROXY_PORT}/?url=http%3A%2F%2F127.0.0.1%3A${MOCK_PORT}%2F" >/dev/null 2>&1; then
        READY=1
        break
    fi
    sleep 0.1
done

if [ "$READY" -ne 1 ]; then
    echo "${C_RED}ERROR: Services failed to initialize within 3 seconds!${C_RESET}"
    exit 1
fi

echo "${C_GREEN}✔ Both Proxy and Upstream are online and responsive.${C_RESET}"
sleep 0.5

# 4. Multi-Phase Warm-up & Peak Benchmark
echo ""
echo "${C_CYAN}[Phase 1/3] Priming upstream V8 JIT & connection pools...${C_RESET}"
./$BIN -m stress -u "http://127.0.0.1:${PROXY_PORT}/?url=http%3A%2F%2F127.0.0.1%3A${MOCK_PORT}%2F" -c ${CONCURRENCY} -n ${TOTAL_REQUESTS} -t ${STRESS_THREADS} >/dev/null 2>&1

echo "${C_CYAN}[Phase 2/3] Warming Linux socket tables & kernel cache lines...${C_RESET}"
./$BIN -m stress -u "http://127.0.0.1:${PROXY_PORT}/?url=http%3A%2F%2F127.0.0.1%3A${MOCK_PORT}%2F" -c ${CONCURRENCY} -n ${TOTAL_REQUESTS} -t ${STRESS_THREADS} >/dev/null 2>&1

echo ""
echo "${C_BOLD}${C_GREEN}🚀 [Phase 3/3] Executing Peak 50,000+ Req/Sec Stress Benchmark...${C_RESET}"
echo "Target       : http://127.0.0.1:${PROXY_PORT}/?url=http://127.0.0.1:${MOCK_PORT}/"
echo "Total Reqs   : ${TOTAL_REQUESTS}"
echo "Concurrency  : ${CONCURRENCY} connections"
echo "Client Fleet : ${STRESS_THREADS} threads"
echo ""

./$BIN -m stress \
  -u "http://127.0.0.1:${PROXY_PORT}/?url=http%3A%2F%2F127.0.0.1%3A${MOCK_PORT}%2F" \
  -c ${CONCURRENCY} \
  -n ${TOTAL_REQUESTS} \
  -t ${STRESS_THREADS}
