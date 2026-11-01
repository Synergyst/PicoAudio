# Dual-board Pico DAC2 build and deployment

This adds a Makefile-based workflow for the two supported boards. It builds
separate firmware images because the Pico 1 is RP2040 and the Pico 2 W is
RP2350; an RP2350 UF2 must not be flashed onto the RP2040 board (or vice versa).

## Commands

```sh
make help
make build             # Build both board-specific UF2 files
make build-pico1       # RP2040 / Raspberry Pi Pico 1
make build-pico2w      # RP2350 / Raspberry Pi Pico 2 W
make deploy            # Build both, detect BOOTSEL volumes, and flash them
make status            # Inspect connected runtime devices and BOOTSEL volumes
make clean             # Remove the two Makefile-managed build directories
```

The builds use GPIO26 for I2S RX DATA and retain GPIO22 for I2S TX DATA. No
additional `-D` parameters are needed when using these Makefile targets.

Output images:

- `build-pico1/mdac_adc2.uf2` — RP2040 / `PICO_BOARD=pico`
- `build-pico2w/mdac_adc2.uf2` — RP2350 / `PICO_BOARD=pico2_w`

## Automatic deployment

`make deploy` first builds both firmware images, then scans USB mass-storage
partitions for the BOOTSEL labels:

- `RPI-RP2` identifies the RP2040 Pico 1 bootloader.
- `RP2350` identifies the RP2350 Pico 2 W bootloader.

It flashes whichever supported board(s) are currently in BOOTSEL. If one or
more of the project's firmware devices are running normally instead, it asks
you to hold BOOTSEL while reconnecting those boards and press Enter to rescan.
It uses a private temporary mount directory under `/run` when a BOOTSEL
partition is not already mounted. If the current device is already mounted,
it uses that exact device's existing mount only after checking that the mount is
readable. It never blindly stacks another mount on `/mnt/pico`; an unreadable
stale mount is reported and no UF2 is copied.

The two boards use the same runtime USB IDs (`cafe:babe`, with the earlier
capture-only build also using `cafe:babf`). Those descriptors do not identify
the silicon family. The Makefile therefore identifies the exact model only
when the board is in BOOTSEL, using its boot-volume label. It counts running
project firmware devices so it can tell whether additional boards still need
to be placed in BOOTSEL.

Deployment expects at most one Pico 1 and one Pico 2 W, matching this setup.
Run `make deploy` as root (or with sudo) because it mounts BOOTSEL volumes.

## Pico 2 W status LED

Unlike the non-wireless Pico 2, the Pico 2 W has no normal GPIO
`PICO_DEFAULT_LED_PIN`; its onboard LED is connected to the CYW43 wireless
chip. This firmware does not initialize the CYW43. The Pico 2 W build therefore
uses no-op status LED functions rather than driving GPIO25, which is the
CYW43 chip-select pin. Audio pins and USB audio behavior are unchanged.

## SDK

The Makefile uses the existing `PICO_SDK_PATH` environment variable and the
project's existing `pico_sdk_import.cmake`. Keep that environment variable set
to the Pico SDK checkout used by this project.
