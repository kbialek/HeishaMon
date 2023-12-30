
compile:
	@bash -c "set -a; source config.env; pio run"

flash:
	@bash -c "set -a; source config.env; pio run -t upload"

flash-ota:
	@bash -c "set -a; source config.env; pio run -t upload -e esp32ota --upload-port 192.168.2.152"

monitor:
	pio device monitor
