#!/bin/bash

set -e

echo "Compiling the main file..."
gcc -O3 -Wall -Wextra -Werror -Iinclude src/*.c -pthread -lssl -lcrypto -o build/api

echo "Running the api file..."
./build/api -p 17777 > logs/api.log &
PID=$!

echo "Server PID: $PID"
echo "Execution done."
