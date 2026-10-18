#!/bin/bash
set -e

PORT=17777
CONCURRENT_CLIENTS=30
REQUESTS_PER_CLIENT=2

mkdir -p logs
echo "=== Memory Benchmark Session $(date) ===" > logs/api.log

echo "==========================================================" | tee -a logs/api.log
echo "     PROXY CONCURRENCY & MEMORY BENCHMARK TOOL            " | tee -a logs/api.log
echo "==========================================================" | tee -a logs/api.log

# 1. Compile
echo "[1/4] Compiling latest binary..." | tee -a logs/api.log
gcc -Wall -Wextra -Iinclude src/main.c src/server.c src/proxy.c src/url.c src/http.c src/logger.c src/tls.c src/http_error.c src/config.c -pthread -lssl -lcrypto -o build/api

# 2. Start server directing output to logs/api.log
echo "[2/4] Launching proxy server on port $PORT..." | tee -a logs/api.log
./build/api -p $PORT >> logs/api.log 2>&1 &
PROXY_PID=$!

cleanup() {
    kill -9 $PROXY_PID 2>/dev/null || true
    rm -f /tmp/proxy_mem_samples.log /tmp/proxy_results.log /tmp/proxy_latencies.log
}
trap cleanup EXIT INT TERM

sleep 0.5
if ! kill -0 $PROXY_PID 2>/dev/null; then
    echo "ERROR: Server failed to start!" | tee -a logs/api.log
    exit 1
fi

get_stat() {
    grep "^$1" /proc/$PROXY_PID/status 2>/dev/null | awk '{print $2}' || echo "0"
}

BASE_VMSIZE=$(get_stat "VmSize:")
BASE_VMRSS=$(get_stat "VmRSS:")
BASE_VMHWM=$(get_stat "VmHWM:")
BASE_THREADS=$(get_stat "Threads:")

echo "  -> Baseline VmRSS (Current RAM):  ${BASE_VMRSS} kB (~$(( BASE_VMRSS / 1024 )) MB)" | tee -a logs/api.log
echo "  -> Baseline VmHWM (High Water):   ${BASE_VMHWM} kB (~$(( BASE_VMHWM / 1024 )) MB)" | tee -a logs/api.log
echo "  -> Baseline Virtual Memory:       ${BASE_VMSIZE} kB" | tee -a logs/api.log
echo "  -> Baseline Threads:              ${BASE_THREADS}" | tee -a logs/api.log

rm -f /tmp/proxy_mem_samples.log /tmp/proxy_results.log /tmp/proxy_latencies.log
touch /tmp/proxy_mem_samples.log /tmp/proxy_results.log /tmp/proxy_latencies.log

# 3. High-Frequency Kernel Memory Poller (every 10ms)
(
    while kill -0 $PROXY_PID 2>/dev/null; do
        R=$(grep "^VmRSS:" /proc/$PROXY_PID/status 2>/dev/null | awk '{print $2}' || echo 0)
        H=$(grep "^VmHWM:" /proc/$PROXY_PID/status 2>/dev/null | awk '{print $2}' || echo 0)
        T=$(grep "^Threads:" /proc/$PROXY_PID/status 2>/dev/null | awk '{print $2}' || echo 0)
        echo "$R $H $T" >> /tmp/proxy_mem_samples.log
        sleep 0.01
    done
) &
MONITOR_PID=$!

echo -e "\n[3/4] Firing $CONCURRENT_CLIENTS concurrent workers (streaming 100KB HTTPS files)..." | tee -a logs/api.log
TOTAL_REQS=$(( CONCURRENT_CLIENTS * REQUESTS_PER_CLIENT ))

START_TIME=$(date +%s%N)

for i in $(seq 1 $CONCURRENT_CLIENTS); do
    (
        for j in $(seq 1 $REQUESTS_PER_CLIENT); do
            # Format: HTTP_CODE TIME_TOTAL (seconds with millisecond precision)
            OUT=$(curl -s -o /dev/null -w "%{http_code} %{time_total}\n" "http://localhost:${PORT}/?url=https://httpbin.org/bytes/100000" || echo "000 0.0")
            CODE=$(echo "$OUT" | awk '{print $1}')
            TIME_SEC=$(echo "$OUT" | awk '{print $2}')
            
            if [ "$CODE" == "200" ]; then
                echo "SUCCESS" >> /tmp/proxy_results.log
                echo "$TIME_SEC" >> /tmp/proxy_latencies.log
            else
                echo "FAILED $CODE" >> /tmp/proxy_results.log
            fi
        done
    ) &
done

# Wait specifically for the client curls to finish
wait $(jobs -p | grep -v $MONITOR_PID | grep -v $PROXY_PID)
END_TIME=$(date +%s%N)
kill $MONITOR_PID 2>/dev/null || true

DURATION_NS=$(( END_TIME - START_TIME ))
DURATION_MS=$(( DURATION_NS / 1000000 ))
DURATION_S=$(awk "BEGIN {print $DURATION_MS / 1000}")

echo "[4/4] Load finished. Waiting 1 second for cooldown and thread cleanup..." | tee -a logs/api.log
sleep 1

POST_VMRSS=$(get_stat "VmRSS:")
FINAL_VMHWM=$(get_stat "VmHWM:")
POST_THREADS=$(get_stat "Threads:")

# Calculate memory metrics
PEAK_SAMPLED_RSS=$(sort -n -k1 /tmp/proxy_mem_samples.log 2>/dev/null | tail -n 1 | awk '{print $1}')
MAX_THREADS=$(sort -n -k3 /tmp/proxy_mem_samples.log 2>/dev/null | tail -n 1 | awk '{print $3}')

# True stress footprint: difference between kernel High-Water Mark and idle baseline
RAM_FOOTPRINT_DELTA=$(( FINAL_VMHWM - BASE_VMRSS ))
RAM_PER_CONN=$(awk "BEGIN {printf \"%.1f\", $RAM_FOOTPRINT_DELTA / $CONCURRENT_CLIENTS}")

TOTAL_SUCCESS=$(grep -c "SUCCESS" /tmp/proxy_results.log 2>/dev/null || echo 0)
TOTAL_FAILED=$(grep -c "FAILED" /tmp/proxy_results.log 2>/dev/null || echo 0)

# Calculate latency stats using python
LATENCY_STATS=$(python3 -c '
import sys
try:
    with open("/tmp/proxy_latencies.log") as f:
        lats = [float(line.strip()) * 1000 for line in f if line.strip()]
    if lats:
        print(f"{min(lats):.1f} {sum(lats)/len(lats):.1f} {max(lats):.1f}")
    else:
        print("0.0 0.0 0.0")
except Exception:
    print("0.0 0.0 0.0")
')

MIN_LAT=$(echo "$LATENCY_STATS" | awk '{print $1}')
AVG_LAT=$(echo "$LATENCY_STATS" | awk '{print $2}')
MAX_LAT=$(echo "$LATENCY_STATS" | awk '{print $3}')

RPS=$(awk "BEGIN {if ($DURATION_MS > 0) printf \"%.1f\", ($TOTAL_SUCCESS / ($DURATION_MS / 1000.0)); else print 0}")

{
echo -e "\n=========================================================="
echo "          STRESS & MEMORY BENCHMARK REPORT                "
echo "=========================================================="
echo "Total Requests:       $TOTAL_REQS ($TOTAL_SUCCESS success, $TOTAL_FAILED failed)"
echo "Peak Active Threads:  $MAX_THREADS (1 listener + $(( MAX_THREADS - 1 )) concurrent workers)"
echo ""
echo "--- ⏱️ CONCURRENCY TIME & THROUGHPUT ---"
echo "Burst Duration:       ${DURATION_MS} ms (${DURATION_S} seconds)"
echo "Throughput:           ${RPS} requests / sec"
echo "Latency (Min / Avg / Max): ${MIN_LAT} ms / ${AVG_LAT} ms / ${MAX_LAT} ms"
echo ""
echo "--- 🧠 KERNEL RAM FOOTPRINT (VmRSS & VmHWM) ---"
echo "Baseline VmRSS (Idle):        ${BASE_VMRSS} kB (~$(( BASE_VMRSS / 1024 )) MB)"
echo "Sampled Peak VmRSS:           ${PEAK_SAMPLED_RSS} kB (~$(( PEAK_SAMPLED_RSS / 1024 )) MB)"
echo "Kernel VmHWM (High Water):    ${FINAL_VMHWM} kB (~$(( FINAL_VMHWM / 1024 )) MB)"
echo "----------------------------------------------------------"
echo "Actual RAM Added By Stress:   ${RAM_FOOTPRINT_DELTA} kB (~$(( RAM_FOOTPRINT_DELTA / 1024 )) MB)"
echo "RAM Footprint Per Connection: ${RAM_PER_CONN} kB / client"
echo "----------------------------------------------------------"
echo "Cooldown VmRSS (After Load):  ${POST_VMRSS} kB"
echo "Active Threads After Load:    ${POST_THREADS} (should be 1)"
echo "=========================================================="
} | tee -a logs/api.log
