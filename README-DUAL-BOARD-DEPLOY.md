# PicoAudio board builds and deployment

PicoAudio builds board-specific firmware for three Raspberry Pi Pico models and two I²S clock roles. Select the board model and role independently:

- Boards: `pico1`, `pico2`, `pico2w`
- Roles: `master`, `slave`

RP2040 and RP2350 firmware images are not interchangeable. Master and slave images also have different clock-pin behavior and must be flashed according to the intended topology.

## Build one board/role combination

Use `BOARD` and `ROLE` with the generic build target:

```sh
make build BOARD=pico1 ROLE=master
make build BOARD=pico2 ROLE=slave
make build BOARD=pico2w ROLE=master
```

The same combinations are available through `build-one`:

```sh
make build-one BOARD=pico2 ROLE=master
```

Convenience targets use the selected `ROLE` variable, defaulting to `master`:

```sh
make build-pico1 [ROLE=master|slave]
make build-pico2 [ROLE=master|slave]
make build-pico2w [ROLE=master|slave]
```

The predefined bridge pair remains available:

```sh
make build-bridge
```

It builds:

- `build-pico2w/mdac_adc2.uf2` — Pico 2 W / RP2350 / master.
- `build-pico2-slave/mdac_adc2.uf2` — Pico 2 (non-W) / RP2350 / slave.

Build output directories are kept separate for each model and role:

| Model | Master build directory | Slave build directory |
|---|---|---|
| Pico 1 | `build-pico1` | `build-pico1-slave` |
| Pico 2 | `build-pico2-master` | `build-pico2-slave` |
| Pico 2 W | `build-pico2w` | `build-pico2w-slave` |

## Interactive deployment

Run deployment as root so the helper can mount BOOTSEL volumes:

```sh
make deploy
```

The helper identifies the silicon family from the BOOTSEL volume label:

- `RPI-RP2` identifies Pico 1 / RP2040 automatically.
- `RP2350` identifies an RP2350 board, but **cannot distinguish Pico 2 from Pico 2 W**. The helper asks which physical model is currently in BOOTSEL.

The helper also asks whether to flash `master` or `slave`, unless you provide that role explicitly. It builds the selected image and flashes only the selected BOOTSEL partition.

For non-interactive selection, specify both values:

```sh
make deploy BOARD=pico1 ROLE=master
make deploy BOARD=pico2 ROLE=slave
make deploy BOARD=pico2w ROLE=master
```

If multiple supported BOOTSEL volumes are connected, the helper asks which device path belongs to the physical board being flashed. Since Pico 2 and Pico 2 W share the same RP2350 bootloader identity, the operator must identify the physical board; software cannot infer the W variant from the BOOTSEL label. The safest procedure is to place only the intended board into BOOTSEL for each deployment.

You can also provide a specific partition path when needed:

```sh
make deploy BOARD=pico2w ROLE=master DEVICE=/dev/sdX1
```

Replace `/dev/sdX1` with the actual partition shown by `make status` or `lsblk`.

`make deploy` builds the selected image automatically. It does not flash every connected board indiscriminately; each invocation flashes one selected BOOTSEL device. Run it again for the next board.

## Status and cleanup

```sh
make status
make clean
```

`make status` lists recognized BOOTSEL volumes and running PicoAudio USB devices. The current USB descriptors do not expose the firmware role, and a running USB device does not identify whether it is Pico 2 or Pico 2 W.

`make clean` removes only the six Makefile-managed board/role build directories.

## Hardware role reminder

The master drives GP20 BCLK and GP21 LRCLK. The slave receives those clocks. Data is crossed in each direction: master GP22 to slave GP26, and slave GP22 to master GP26. Connect GND to GND. Keep the boards independently USB-powered; do not connect VBUS, VSYS, or 3V3 between them.

The master must have an active USB playback stream to generate I²S clocks. Both boards should use the same nominal sample rate. See [README-DUAL-BOARD-ROLES.md](README-DUAL-BOARD-ROLES.md) for the wiring and staged audio tests.
