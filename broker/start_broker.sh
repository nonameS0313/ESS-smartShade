#!/bin/sh
# Start Mosquitto broker for Smart Shade (run on Pi2)
DIR="$(cd "$(dirname "$0")" && pwd)"
exec mosquitto -c "$DIR/mosquitto.conf" -v
