#!/bin/bash
set -e

# ==============================================================================
# scripts/test_kqueue.sh
# Cross-Platform Kqueue Test Runner on Linux (powered by local libkqueue)
# ==============================================================================

PORT=17777
SCRATCH_DIR="/home/himanshu-yadav/.gemini/antigravity-ide/brain/d4c4fd88-680f-4fa7-a457-8b3362faaee5/scratch/kqueue_install"

echo "================================================================"
echo "   BUILDING & TESTING C PROXY WITH KQUEUE / BSD ENGINE          "
echo "================================================================"

# 1. Clean previous logs
echo "[1/4] Clearing previous log files..."
rm -f logs/@*.log logs/logger_*.log

# 2. Compile with libkqueue
echo "[2/4] Compiling with kqueue engine (-O3 -flto)..."
gcc -O3 -flto -DTEST_KQUEUE_ON_LINUX \
    -Iinclude \
    -I"${SCRATCH_DIR}/include/kqueue" \
    src/main.c src/proxy.c src/server.c src/logger.c src/config.c \
    src/http.c src/http_error.c src/tls.c src/upstream.c src/url.c \
    src/poll_kqueue.c \
    "${SCRATCH_DIR}/lib/libkqueue.a" \
    -pthread -lssl -lcrypto \
    -o build/api_kqueue

# 3. Start background server
echo "[3/4] Launching background server on port ${PORT}..."
./build/api_kqueue -p ${PORT} &
SERVER_PID=$!

cleanup() {
    echo -e "\nStopping kqueue server (PID: ${SERVER_PID})..."
    kill -TERM "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
}
trap cleanup EXIT

# Give the server 300ms to bind
sleep 0.3

# 4. Run test suite
echo "[4/4] Firing real-world WAN test suite against kqueue..."
node scripts/test_suite.js

echo -e "\nFiring 20,000-request high-throughput benchmark against kqueue..."
node scripts/local_benchmark.js

echo "================================================================"
echo "   KQUEUE TEST & BENCHMARK COMPLETE!                            "
echo "================================================================"
