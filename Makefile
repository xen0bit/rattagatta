PIO := uv run pio

.PHONY: all build-collector build-logger clean flash-collector flash-logger sync

all: build-collector build-logger

build-collector:
	cd rg-collector && $(PIO) run

build-logger:
	cd rg-logger && $(PIO) run

# Usage: make flash-collector PORT=/dev/ttyUSB0
flash-collector:
	cd rg-collector && $(PIO) run --target upload --upload-port $(PORT)

# Usage: make flash-logger PORT=/dev/ttyUSB0
flash-logger:
	cd rg-logger && $(PIO) run --target upload --upload-port $(PORT)

# Usage: make monitor-collector PORT=/dev/ttyUSB0
monitor-collector:
	cd rg-collector && $(PIO) device monitor --port $(PORT) --baud 115200 --filter esp32_exception_decoder

# Usage: make monitor-logger PORT=/dev/ttyUSB0
monitor-logger:
	cd rg-logger && $(PIO) device monitor --port $(PORT) --baud 115200

clean:
	cd rg-collector && $(PIO) run --target clean
	cd rg-logger && $(PIO) run --target clean

# Install pinned dependencies via uv
sync:
	uv sync
