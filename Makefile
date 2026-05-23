PIO            := uv run pio
COLLECTOR_ENV  := seeed_xiao_esp32s3
COLLECTOR_BIN  := rg-collector/.pio/build/$(COLLECTOR_ENV)/firmware.bin
COLLECTOR_FW_H := rg-logger/include/collector_firmware.h

.PHONY: all build-collector embed-firmware build-logger clean \
        flash-collector flash-logger monitor-collector monitor-logger sync

# Default: build both (collector first, then embed into logger)
all: build-logger

build-collector:
	cd rg-collector && $(PIO) run

# Convert collector .bin → PROGMEM byte array included by the logger.
# xxd -i with stdin emits raw bytes only (no declaration wrapper), so we add
# the declaration and closing brace around it manually.
embed-firmware: build-collector
	@echo "Embedding $(COLLECTOR_BIN) → $(COLLECTOR_FW_H)"
	@echo '#pragma once'                                           > $(COLLECTOR_FW_H)
	@echo '#include <Arduino.h>'                                  >> $(COLLECTOR_FW_H)
	@echo 'static const uint8_t collector_firmware[] PROGMEM = {' >> $(COLLECTOR_FW_H)
	@xxd -i < $(COLLECTOR_BIN)                                    >> $(COLLECTOR_FW_H)
	@echo '};'                                                    >> $(COLLECTOR_FW_H)
	@printf 'static const size_t collector_firmware_len = %d;\n' \
	    $$(wc -c < $(COLLECTOR_BIN))                              >> $(COLLECTOR_FW_H)

build-logger: embed-firmware
	cd rg-logger && $(PIO) run

# Usage: make flash-collector PORT=/dev/ttyUSB0
flash-collector:
	cd rg-collector && $(PIO) run --target upload --upload-port $(PORT)

# Usage: make flash-logger PORT=/dev/ttyUSB0
# Rebuilds collector + embeds firmware before flashing so the logger always
# carries the binary that matches its COLLECTOR_FW_VERSION constant.
flash-logger: embed-firmware
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
	rm -f $(COLLECTOR_FW_H)

# Install pinned dependencies via uv
sync:
	uv sync
