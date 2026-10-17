#!/bin/bash

set -e

echo "Compiling the main file..."
gcc -Wall -Wextra -Iinclude src/main.c src/server.c src/proxy.c src/url.c src/http.c src/logger.c src/tls.c src/http_error.c src/config.c -lssl -lcrypto -o build/api

echo "Running the api file..."
./build/api -p 17777 -m logs > logs/api.log &
PID=$!

echo "Server PID: $PID"
echo "Execution done."
