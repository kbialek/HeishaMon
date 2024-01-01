#!/usr/bin/env bash

set -uf -o pipefail

function read_raw_data_line() {
    mosquitto_sub -h "$MQTT_SERVER" -u "$MQTT_USERNAME" -P "$MQTT_PASSWORD" -F '%x' -C 1 -t 'home/hvac/heatpump/board/raw/data'
}

while true; do
    echo $(read_raw_data_line)
done