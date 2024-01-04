
compile:
	@bash -c "set -a; source config.env; pio run"

flash:
	@bash -c "set -a; source config.env; pio run -t upload"

flash-ota:
	@bash -c "set -a; source config.env; pio run -t upload -e esp32ota"

monitor:
	pio device monitor

print-raw-data:
	@bash -c "set -a; source config.env; ./bin/print-raw-data.sh"

print-logs:
	@bash -c "set -a; source config.env; ./bin/print-logs.sh"