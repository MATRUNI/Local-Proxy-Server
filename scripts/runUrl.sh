#!/bin/bash
set -e

echo "Compiling the main file..."

gcc -O2 src/test_url.c \
    -o build/url_test

echo "Running the url parser..."

./build/url_test

echo "Execution done."
