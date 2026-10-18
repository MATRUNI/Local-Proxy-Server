#!/bin/bash
echo "Stopping proxy server..."
pkill -SIGINT api || echo "Server is not running."
