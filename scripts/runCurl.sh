#!/bin/bash
set -e

mkdir -p logs

PORT=17777

echo "Firing GET request through proxy..."
curl -i -s "http://localhost:${PORT}/?url=http://httpbin.org/get" \
  -H "Origin: http://localhost:3000" > logs/response.log 2>&1

echo "Firing POST request through proxy..."
curl -i -s -X POST "http://localhost:${PORT}/?url=http://httpbin.org/post" \
  -H "Content-Type: application/json" \
  -H "Origin: http://localhost:3000" \
  -d '{"test": "POST request working"}' >> logs/response.log 2>&1

echo "Firing PUT request through proxy..."
curl -i -s -X PUT "http://localhost:${PORT}/?url=http://httpbin.org/put" \
  -H "Content-Type: application/json" \
  -H "Origin: http://localhost:3000" \
  -d '{"test": "PUT request working"}' >> logs/response.log 2>&1

echo "Firing DELETE request through proxy..."
curl -i -s -X DELETE "http://localhost:${PORT}/?url=http://httpbin.org/delete" \
  -H "Origin: http://localhost:3000" >> logs/response.log 2>&1

echo "Firing OPTIONS preflight request..."
curl -i -s -X OPTIONS "http://localhost:${PORT}/" \
  -H "Origin: http://localhost:3000" \
  -H "Access-Control-Request-Method: POST" \
  -H "Access-Control-Request-Headers: authorization" >> logs/response.log 2>&1

echo "Done! Check logs/response.log for the output."
