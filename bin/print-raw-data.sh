#!/usr/bin/env bash

set -uf -o pipefail

mosquitto_sub -h "$MQTT_SERVER" -u "$MQTT_USERNAME" -P "$MQTT_PASSWORD" -F '%t : %x' -t 'home/hvac/heatpump/board/raw/#'
