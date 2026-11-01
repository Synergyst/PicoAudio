SHELL := /bin/bash
.DEFAULT_GOAL := help

CMAKE ?= cmake
JOBS ?= $(shell nproc 2>/dev/null || echo 4)
PICO_SDK_PATH ?= $(shell printenv PICO_SDK_PATH)

PICO1_BUILD_DIR := build-pico1
PICO2W_BUILD_DIR := build-pico2w
RX_DATA_PIN ?= 26

.PHONY: help all build build-pico1 build-pico2w deploy flash status clean

help:
	@printf '%s\n' \
	  'Pico DAC2 full-duplex firmware' \
	  '' \
	  'Build targets:' \
	  '  make build          Build both Pico 1 (RP2040) and Pico 2 W (RP2350)' \
	  '  make build-pico1    Build only the original Pico 1' \
	  '  make build-pico2w   Build only the Pico 2 W' \
	  '' \
	  'Deployment targets:' \
	  '  make deploy         Build both, detect BOOTSEL boards, and flash them' \
	  '  make flash          Alias for make deploy' \
	  '  make status         Show running firmware devices and BOOTSEL volumes' \
	  '' \
	  'Maintenance:' \
	  '  make clean          Remove both generated build directories' \
	  '' \
	  'Optional: make JOBS=4 build   Limit parallel build jobs.'

all: build
build: build-pico1 build-pico2w

build-pico1:
	$(CMAKE) -S . -B $(PICO1_BUILD_DIR) \
	  -DBOARD_NAME=rp2040 \
	  -DPICO_BOARD=pico \
	  -DPICODAC_I2S_RX_DATA_PIN=$(RX_DATA_PIN)
	$(CMAKE) --build $(PICO1_BUILD_DIR) -j$(JOBS)

build-pico2w:
	$(CMAKE) -S . -B $(PICO2W_BUILD_DIR) \
	  -DBOARD_NAME=rp2350 \
	  -DPICO_BOARD=pico2_w \
	  -DPICODAC_I2S_RX_DATA_PIN=$(RX_DATA_PIN)
	$(CMAKE) --build $(PICO2W_BUILD_DIR) -j$(JOBS)

deploy: build
	bash tools/deploy-picos.sh deploy $(PICO1_BUILD_DIR)/mdac_adc2.uf2 $(PICO2W_BUILD_DIR)/mdac_adc2.uf2

flash: deploy

status:
	bash tools/deploy-picos.sh status

clean:
	rm -rf $(PICO1_BUILD_DIR) $(PICO2W_BUILD_DIR)
