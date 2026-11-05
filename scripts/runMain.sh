#!/bin/bash

set -e

echo "Compiling Sankram..."
gcc -O3 -flto -march=native -Wall -Wextra -Werror -Iinclude src/*.c -pthread -lssl -lcrypto -o build/sankram

echo "Running Sankram..."
./build/sankram -p 17777 > logs/sankram.log &
PID=$!

echo "Sankram PID: $PID"
echo "Execution done."
