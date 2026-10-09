#!/bin/bash
set -e

mkdir -p logs

echo "Firing GET request through proxy..."
curl -i -s -x http://localhost:4000 http://httpbin.org/get > logs/response.log 2>&1

echo "Firing OPTIONS preflight request..."
curl -i -s -X OPTIONS http://localhost:4000/get \
  -H "Origin: http://localhost:3000" \
  -H "Access-Control-Request-Method: POST" \
  -H "Access-Control-Request-Headers: authorization" >> logs/response.log 2>&1

echo "Done! Check logs/response.log for the output."