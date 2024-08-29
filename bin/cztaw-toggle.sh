#!/usr/bin/env bash

set -uf -o pipefail

mosquitto_pub -h "$MQTT_SERVER" -u "$MQTT_USERNAME" -P "$MQTT_PASSWORD" -t 'home/hvac/heatpump/board/control/cztaw_rw' -m "$1" -q 1
