SHELL := /bin/bash
.DEFAULT_GOAL := help

CMAKE ?= cmake
JOBS ?= $(shell nproc 2>/dev/null || echo 4)

# The Stage-1 C GPIO diagnostic is an RP2350 build.
BOARD ?=
ROLE ?=
BUILD_ROLE := $(if $(strip $(ROLE)),$(strip $(ROLE)),master)
DEVICE ?=

.PHONY: help all build build-one build-pico2 build-pico2-slave \
        build-pico2w build-pico2w-slave build-bridge deploy flash status clean

help:
	@printf '%s\n' \
	  'PicoAudio RP2350 GPIO C diagnostic firmware (PCM transport disabled)' \
	  '' \
	  'Generic build:' \
	  '  make build BOARD=pico2 ROLE=slave' \
	  '  make build BOARD=pico2w ROLE=master' \
	  '  make build-one BOARD=<pico2|pico2w> ROLE=<master|slave>' \
	  '' \
	  'Convenience builds (ROLE defaults to master):' \
	  '  make build-pico2 [ROLE=master|slave]' \
	  '  make build-pico2w [ROLE=master|slave]' \
	  '  make build-bridge       Build Pico 2 W master + Pico 2 W slave' \
	  '  make build              Build BOARD/ROLE if BOARD is set; otherwise bridge pair' \
	  '' \
	  'Interactive / explicit deployment (run as root):' \
	  '  make deploy BOARD=pico2w ROLE=slave' \
	  '  make deploy BOARD=pico2w ROLE=master' \
	  '  make status             Show BOOTSEL volumes and running PicoAudio devices' \
	  '' \
	  'This C-only GPIO diagnostic requires RP2350 (Pico 2 / Pico 2 W).' \
	  'It verifies the 1:1 GPIO link before exposing UAC2; PCM forwarding is disabled.'

all: build

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
	  pico2) soc=rp2350; pico_board=pico2 ;; \
	  pico2w) soc=rp2350; pico_board=pico2_w ;; \
	  *) echo "Unsupported BOARD='$$board'. This C GPIO diagnostic requires pico2 or pico2w (RP2350)." >&2; exit 2 ;; \
	esac; \
	case "$$role" in master|slave) ;; *) echo "Unsupported ROLE='$$role'. Use master or slave." >&2; exit 2 ;; esac; \
	case "$$board-$$role" in \
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
	echo "Building PicoAudio: board=$$board, SoC=$$soc, Pico SDK board=$$pico_board, GPIO bitbang role=$$role"; \
	$(CMAKE) -S . -B "$$build_dir" \
	  -DBOARD_NAME="$$soc" \
	  -DPICO_BOARD="$$pico_board" \
	  -DPICODAC_LINK_ROLE="$$role"; \
	$(CMAKE) --build "$$build_dir" --parallel '$(JOBS)'; \
	echo "UF2: $$build_dir/mdac_adc2.uf2"

build-pico2:
	$(MAKE) --no-print-directory build-one BOARD=pico2 ROLE='$(BUILD_ROLE)'

build-pico2-slave:
	$(MAKE) --no-print-directory build-one BOARD=pico2 ROLE=slave

build-pico2w:
	$(MAKE) --no-print-directory build-one BOARD=pico2w ROLE='$(BUILD_ROLE)'

build-pico2w-slave:
	$(MAKE) --no-print-directory build-one BOARD=pico2w ROLE=slave

build-bridge:
	$(MAKE) --no-print-directory build-one BOARD=pico2w ROLE=master
	$(MAKE) --no-print-directory build-one BOARD=pico2w ROLE=slave

deploy:
	@BOARD='$(strip $(BOARD))' ROLE='$(strip $(ROLE))' DEVICE='$(strip $(DEVICE))' \
	  JOBS='$(JOBS)' CMAKE='$(CMAKE)' \
	  bash tools/deploy-picos.sh deploy

flash: deploy

status:
	bash tools/deploy-picos.sh status

clean:
	rm -rf -- \
	  build-pico1 build-pico1-slave \
	  build-pico2-master build-pico2-slave \
	  build-pico2w build-pico2w-slave
