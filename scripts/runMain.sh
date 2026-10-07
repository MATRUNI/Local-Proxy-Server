#!/bin/bash

set -e

echo "Compiling the main file..."
gcc -Wall -Wextra -Iinclude src/main.c src/server.c src/proxy.c src/url.c src/http.c -o build/api

echo "Running the api file..."
./build/api -p 4000 -n namesind > logs/api.log &
PID=$!

echo "Server PID: $PID"
echo "Execution done."
