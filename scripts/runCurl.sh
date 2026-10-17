#!/bin/bash
set -e

mkdir -p logs

PORT=17777

echo "=== Clearing old logs ==="
> logs/response.log

echo "--- 1. Testing Plain HTTP GET ---"
echo -e "\n=== 1. Plain HTTP GET ===" >> logs/response.log
curl -i -s "http://localhost:${PORT}/?url=http://httpbin.org/get" \
  -H "Origin: http://localhost:3000" >> logs/response.log 2>&1

echo "--- 2. Testing Plain HTTP POST ---"
echo -e "\n=== 2. Plain HTTP POST ===" >> logs/response.log
curl -i -s -X POST "http://localhost:${PORT}/?url=http://httpbin.org/post" \
  -H "Content-Type: application/json" \
  -H "Origin: http://localhost:3000" \
  -d '{"test": "HTTP POST working"}' >> logs/response.log 2>&1

echo "--- 3. Testing HTTPS GET (TLS Handshake & Decryption) ---"
echo -e "\n=== 3. HTTPS GET ===" >> logs/response.log
curl -i -s "http://localhost:${PORT}/?url=https://httpbin.org/get" \
  -H "Origin: http://localhost:3000" >> logs/response.log 2>&1

echo "--- 4. Testing HTTPS POST (Encrypted Upload & Download) ---"
echo -e "\n=== 4. HTTPS POST ===" >> logs/response.log
curl -i -s -X POST "http://localhost:${PORT}/?url=https://httpbin.org/post" \
  -H "Content-Type: application/json" \
  -H "Origin: http://localhost:3000" \
  -d '{"test": "HTTPS POST working securely"}' >> logs/response.log 2>&1

echo "--- 5. Testing OPTIONS preflight ---"
echo -e "\n=== 5. OPTIONS Preflight ===" >> logs/response.log
curl -i -s -X OPTIONS "http://localhost:${PORT}/" \
  -H "Origin: http://localhost:3000" \
  -H "Access-Control-Request-Method: POST" \
  -H "Access-Control-Request-Headers: authorization" >> logs/response.log 2>&1

echo "--- 6. Testing Concurrent Requests (pthreads in parallel) ---"
echo -e "\n=== 6. Concurrent Parallel Requests ===" >> logs/response.log
echo "Firing 10 simultaneous requests..."
for i in {1..10}; do
  (
    CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:${PORT}/?url=https://httpbin.org/get")
    echo "  [Worker Thread] Request $i finished with HTTP $CODE"
    echo "Request $i: HTTP $CODE" >> logs/response.log
  ) &
done
wait

echo "Done! Check logs/response.log for the full results."
