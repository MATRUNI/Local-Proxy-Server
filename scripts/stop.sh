#!/bin/bash
echo "Stopping proxy server..."
pkill -SIGINT sankram || pkill -SIGINT api || echo "Server is not running."
