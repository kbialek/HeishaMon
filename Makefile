
compile:
	@bash -c "set -a; source config.env; pio run"

flash:
	@bash -c "set -a; source config.env; pio run -t upload"

flash-ota:
	@bash -c "set -a; source config.env; pio run -t upload -e esp32ota"

monitor:
	pio device monitor
