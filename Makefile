SHELL := /bin/bash
.DEFAULT_GOAL := help

CMAKE ?= cmake
JOBS ?= $(shell nproc 2>/dev/null || echo 4)
RX_DATA_PIN ?= 26

# Leave BOARD empty for the default bridge build. ROLE defaults to master for
# build targets, while deploy prompts for a role unless ROLE is explicitly set.
BOARD ?=
ROLE ?=
BUILD_ROLE := $(if $(strip $(ROLE)),$(strip $(ROLE)),master)
DEVICE ?=

.PHONY: help all build build-one build-pico1 build-pico1-slave \
        build-pico2 build-pico2-slave build-pico2w build-pico2w-slave \
        build-bridge deploy flash status clean

help:
	@printf '%s\n' \
	  'PicoAudio full-duplex USB audio firmware' \
	  '' \
	  'Generic build:' \
	  '  make build BOARD=pico1 ROLE=master' \
	  '  make build BOARD=pico2 ROLE=slave' \
	  '  make build BOARD=pico2w ROLE=master' \
	  '  make build-one BOARD=<pico1|pico2|pico2w> ROLE=<master|slave>' \
	  '' \
	  'Convenience builds (ROLE defaults to master):' \
	  '  make build-pico1 [ROLE=master|slave]' \
	  '  make build-pico2 [ROLE=master|slave]' \
	  '  make build-pico2w [ROLE=master|slave]' \
	  '  make build-bridge       Build Pico 2 W master + Pico 2 slave' \
	  '  make build              Build BOARD/ROLE if BOARD is set; otherwise bridge pair' \
	  '' \
	  'Interactive / explicit deployment (run as root):' \
	  '  make deploy' \
	  '  make deploy BOARD=pico1 ROLE=master' \
	  '  make deploy BOARD=pico2 ROLE=slave' \
	  '  make deploy BOARD=pico2w ROLE=master' \
	  '  make status             Show BOOTSEL volumes and running PicoAudio devices' \
	  '' \
	  'Pico 1 is identified by the RPI-RP2 BOOTSEL label. Pico 2 and Pico 2 W' \
	  'both show RP2350, so deployment asks which model is in BOOTSEL unless' \
	  'BOARD=pico2 or BOARD=pico2w is supplied. ROLE is also prompted unless set.' \
	  '' \
	  'Optional: make JOBS=7 build-bridge RX_DATA_PIN=26'

all: build

# A generic `make build` keeps the two-board development pair as its default.
# Supplying BOARD selects exactly one board/role image instead.
build:
	@if [[ -n "$(strip $(BOARD))" ]]; then \
	  $(MAKE) --no-print-directory build-one BOARD='$(BOARD)' ROLE='$(BUILD_ROLE)'; \
	else \
	  $(MAKE) --no-print-directory build-bridge; \
	fi

build-one:
	@set -euo pipefail; \
	board='$(strip $(BOARD))'; role='$(BUILD_ROLE)'; \
	case "$$board" in \
	  pico1) soc=rp2040; pico_board=pico ;; \
	  pico2) soc=rp2350; pico_board=pico2 ;; \
	  pico2w) soc=rp2350; pico_board=pico2_w ;; \
	  *) echo "Unsupported BOARD='$$board'. Use pico1, pico2, or pico2w." >&2; exit 2 ;; \
	esac; \
	case "$$role" in master|slave) ;; *) echo "Unsupported ROLE='$$role'. Use master or slave." >&2; exit 2 ;; esac; \
	case "$$board-$$role" in \
	  pico1-master) build_dir=build-pico1 ;; \
	  pico1-slave) build_dir=build-pico1-slave ;; \
	  pico2-master) build_dir=build-pico2-master ;; \
	  pico2-slave) build_dir=build-pico2-slave ;; \
	  pico2w-master) build_dir=build-pico2w ;; \
	  pico2w-slave) build_dir=build-pico2w-slave ;; \
	esac; \
	if [[ -f "$$build_dir/CMakeCache.txt" ]]; then \
	  cached_source="$$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$$build_dir/CMakeCache.txt" | tail -n 1)"; \
	  if [[ -n "$$cached_source" && "$$cached_source" != "$(CURDIR)" ]]; then \
	    echo "Removing stale CMake cache in $$build_dir (source moved from $$cached_source)."; \
	    rm -rf -- "$$build_dir"; \
	  fi; \
	fi; \
	echo "Building PicoAudio: board=$$board, SoC=$$soc, Pico SDK board=$$pico_board, I2S role=$$role"; \
	$(CMAKE) -S . -B "$$build_dir" \
	  -DBOARD_NAME="$$soc" \
	  -DPICO_BOARD="$$pico_board" \
	  -DPICODAC_I2S_ROLE="$$role" \
	  -DPICODAC_I2S_RX_DATA_PIN='$(RX_DATA_PIN)'; \
	$(CMAKE) --build "$$build_dir" --parallel '$(JOBS)'; \
	echo "UF2: $$build_dir/mdac_adc2.uf2"

build-pico1:
	$(MAKE) --no-print-directory build-one BOARD=pico1 ROLE='$(BUILD_ROLE)'

build-pico1-slave:
	$(MAKE) --no-print-directory build-one BOARD=pico1 ROLE=slave

build-pico2:
	$(MAKE) --no-print-directory build-one BOARD=pico2 ROLE='$(BUILD_ROLE)'

build-pico2-slave:
	$(MAKE) --no-print-directory build-one BOARD=pico2 ROLE=slave

build-pico2w:
	$(MAKE) --no-print-directory build-one BOARD=pico2w ROLE='$(BUILD_ROLE)'

build-pico2w-slave:
	$(MAKE) --no-print-directory build-one BOARD=pico2w ROLE=slave

# Known-good intended pairing: Pico 2 W generates clocks; Pico 2 follows them.
build-bridge:
	$(MAKE) --no-print-directory build-one BOARD=pico2w ROLE=master
	$(MAKE) --no-print-directory build-one BOARD=pico2 ROLE=slave

# Deploys exactly one BOOTSEL device. The script detects RP2040 automatically,
# prompts for Pico 2 vs Pico 2 W on RP2350, and prompts for master/slave unless
# BOARD and/or ROLE are supplied on the make command line.
deploy:
	@BOARD='$(strip $(BOARD))' ROLE='$(strip $(ROLE))' DEVICE='$(strip $(DEVICE))' \
	  JOBS='$(JOBS)' RX_DATA_PIN='$(RX_DATA_PIN)' CMAKE='$(CMAKE)' \
	  bash tools/deploy-picos.sh deploy

flash: deploy

status:
	bash tools/deploy-picos.sh status

clean:
	rm -rf -- \
	  build-pico1 build-pico1-slave \
	  build-pico2-master build-pico2-slave \
	  build-pico2w build-pico2w-slave
