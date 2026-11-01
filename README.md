# PicoAudio

**USB Audio Class 2.0 full-duplex audio firmware for Raspberry Pi Pico boards, with PIO/DMA-based I²S playback and capture.**

PicoAudio is a development project built around the RP2040 and RP2350 microcontrollers. It presents a Raspberry Pi Pico as a USB stereo audio device with simultaneous playback and capture, and moves audio samples between USB and I²S using the Pico's PIO and DMA hardware.

> **Project status:** USB playback and capture have both been tested on one board at the same time. The I²S transmit-to-receive loopback has also been tested on a single Pico 2 W. The two-board I²S bridge is a planned next step; the current firmware does not yet implement separate I²S clock-master and clock-slave roles. Do not connect two identically configured boards' BCLK/LRCLK outputs together.

## Features

- USB Audio Class 2.0 (UAC2) stereo playback and capture.
- Simultaneous USB playback and capture on one device.
- 44.1 kHz and 48 kHz sample rates.
- Playback formats:
  - 16-bit signed PCM (`S16_LE`)
  - 24-bit packed PCM (`S24_3LE`)
  - 32-bit signed PCM (`S32_LE`)
- Capture format: 32-bit signed stereo PCM (`S32_LE`).
- 32-bit I²S slots for playback, including when the selected USB playback format is 16-bit or packed 24-bit.
- PIO/DMA-based I²S transmit and receive paths.
- Asynchronous USB playback with an explicit feedback endpoint.
- Full-speed USB packet sizing intended to avoid the bandwidth errors seen with larger isochronous packets.
- Build targets for RP2040 and RP2350 boards.
- Make-based build and BOOTSEL deployment helpers for the currently integrated Pico 1 and Pico 2 W targets.

## Supported boards

| Board | MCU | Build configuration | Status |
|---|---|---|---|
| Raspberry Pi Pico | RP2040 | `BOARD_NAME=rp2040`, `PICO_BOARD=pico` | Build-tested; playback/capture firmware path tested |
| Raspberry Pi Pico 2 | RP2350 | `BOARD_NAME=rp2350`, `PICO_BOARD=pico2` | Target configuration documented; validate the build and hardware on this board |
| Raspberry Pi Pico 2 W | RP2350 | `BOARD_NAME=rp2350`, `PICO_BOARD=pico2_w` | Build-tested and tested on hardware |

The Pico 2 and Pico 2 W use the same RP2350 family. PicoAudio does not use Wi-Fi, so the non-W Pico 2 does not need wireless hardware or wireless configuration. The non-W board is selected with the Pico SDK board name `pico2`; the W board uses `pico2_w`.

The Makefile currently provides the integrated `build-pico1` and `build-pico2w` targets. Until a dedicated non-W Pico 2 Makefile/deployment target is added, build that board directly with CMake as shown below. The Pico SDK uses `pico2` as the board identifier for Raspberry Pi Pico 2. See the [Raspberry Pi Pico C/C++ SDK documentation](https://www.raspberrypi.com/documentation/microcontrollers/c_sdk.html).

## USB audio format and bandwidth notes

The USB interface is configured for full-speed USB. Its isochronous endpoint sizing is limited accordingly; the current implementation uses a maximum packet buffer of 392 bytes (49 stereo frames when each frame is two 32-bit samples).

The 24-bit playback alternate setting is advertised as packed three-byte samples (`S24_3LE`). Advertising 24 significant bits in a four-byte subslot can be interpreted by Linux as a 32-bit sample container, which can result in the wrong alternate setting being selected. PicoAudio converts the supported USB playback formats into the 32-bit I²S sample slots used by the transmit path.

Only 44.1 kHz and 48 kHz are currently enabled. Higher sample rates and high-speed USB are not supported by this full-speed firmware configuration.

## I²S pins

The current pin assignment is:

| Signal | GPIO | Direction in the current single-board test |
|---|---:|---|
| BCLK | GP20 | Output from I²S transmitter; shared with the receive timing path |
| LRCLK | GP21 | Output from I²S transmitter; shared with the receive timing path |
| I²S transmit data | GP22 | Output |
| I²S receive data | GP26 | Input |

### Single-board loopback test

The verified loopback connection is:

```text
GP22 (I²S TX DATA)  ─────  GP26 (I²S RX DATA)
GP20 (BCLK)         ─────  shared clock net
GP21 (LRCLK)        ─────  shared clock net
```

On the single board, the transmitter generates BCLK and LRCLK and the receiver uses those clocks. The data jumper from GP22 to GP26 closes the digital loop. USB playback can therefore be sent out through I²S and captured back through the USB capture interface.

The single-board loopback has been tested on a Pico 2 W, including a clear left/right alternating test tone and simultaneous USB playback/capture.

### Two-board bridge: not implemented yet

A two-board full-duplex bridge requires distinct clock roles:

- One board must be the I²S clock master and drive BCLK/LRCLK.
- The other board must receive those clocks and run its I²S transmit/receive PIO state machines as a clock slave.
- The current firmware does not yet provide this master/slave split. Both boards currently use the clock-generating transmit implementation.

**Do not connect the BCLK or LRCLK pins of two boards running the current firmware together.** That would connect two clock outputs and can cause electrical contention. A future master/slave firmware change is required before the two-board wiring can be used.

The intended data connections for a future two-board design are:

```text
Board A TX DATA (GP22)  ─────  Board B RX DATA (GP26)
Board B TX DATA (GP22)  ─────  Board A RX DATA (GP26)
Board A BCLK  (GP20)    ─────  Board B BCLK  (GP20)  [A is master]
Board A LRCLK (GP21)    ─────  Board B LRCLK (GP21) [A is master]
GND                     ─────  GND
```

This is the planned signal topology, not a currently supported connection diagram. Keep the boards independently USB-powered. Do not tie their VBUS, VSYS, or 3V3 rails together.

## Building

### Requirements

- Raspberry Pi Pico SDK. The project configuration used during development was SDK 2.2.0.
- CMake and GNU Make.
- ARM GNU embedded toolchain (`arm-none-eabi-gcc`).
- `PICO_SDK_PATH` set to the Pico SDK directory, unless your local Pico SDK setup provides it another way.

Example environment setup:

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
```

### Build the integrated targets

From the repository root:

```sh
make help
make build
```

The current Makefile also provides:

```sh
make build-pico1
make build-pico2w
make status
make deploy
make clean
```

Build output locations:

```text
build-pico1/mdac_adc2.uf2
build-pico2w/mdac_adc2.uf2
```

`make deploy` builds the configured targets and flashes boards that the deployment helper detects in BOOTSEL mode. Check `make help` and `tools/deploy-picos.sh` for the exact behavior of the checked-out version.

### Build a non-W Raspberry Pi Pico 2

The Pico 2 uses the RP2350 target with `PICO_BOARD=pico2`. Until the Makefile has a dedicated target for it, configure and build it directly:

```sh
cmake -S . -B build-pico2 \
  -DBOARD_NAME=rp2350 \
  -DPICO_BOARD=pico2 \
  -DPICODAC_I2S_RX_DATA_PIN=26

cmake --build build-pico2 --parallel 7
```

The resulting UF2 is expected at:

```text
build-pico2/mdac_adc2.uf2
```

Put the Pico 2 into BOOTSEL mode, connect it over USB, and copy that UF2 to its RP2350 mass-storage volume. This non-W Pico 2 build path still needs to be compiled and hardware-tested on the newly added board before it should be considered validated.

### Flashing manually

1. Hold **BOOTSEL** while connecting the Pico to USB.
2. Wait for the board's USB mass-storage volume to appear.
3. Copy the UF2 built for that exact board variant to the volume.
4. The board should reboot and enumerate as a USB audio device.

Do not flash an RP2040 UF2 to an RP2350 board or an RP2350 UF2 to an RP2040 board. Pico 2 and Pico 2 W are both RP2350, but use their respective `PICO_BOARD` settings when building.

## Testing on Linux

The following are useful for confirming USB enumeration and stream operation:

```sh
aplay -l
arecord -l
```

For a playback test, select the PicoAudio playback device in your ALSA/PipeWire application or use `speaker-test` with the device name reported by ALSA. To check capture, record a short WAV with `arecord` using the device and format reported by `arecord -l` / `arecord --dump-hw-params`.

Playback and capture have been tested concurrently on one Pico device. Exact ALSA card numbers and device names depend on the host and the order in which USB audio devices are connected.

## Repository layout

| Path | Purpose |
|---|---|
| `main.c` | Firmware entry point and audio subsystem setup |
| `audio_device.c/.h` | USB audio device / clock handling |
| `audio_capture.c/.h` | Capture-side audio handling |
| `i2s.c`, `i2s.pio` | I²S transmit implementation |
| `i2s_rx.c/.h`, `i2s_rx.pio` | I²S receive implementation |
| `usb_audio.c` | USB playback handling and sample conversion |
| `usb_audio_capture.c/.h` | USB capture streaming |
| `usb_descriptor.h` | Composite USB audio/HID descriptors |
| `Makefile` | Build and deployment entry points |
| `tools/deploy-picos.sh` | BOOTSEL detection and UF2 deployment helper |

Additional implementation notes are available in:

- [`README-DUPLEX.md`](README-DUPLEX.md)
- [`README-FULL-SPEED-DUPLEX.md`](README-FULL-SPEED-DUPLEX.md)
- [`README-DUAL-BOARD-DEPLOY.md`](README-DUAL-BOARD-DEPLOY.md)

## USB identifiers

The current firmware uses these default USB identifiers:

- Vendor ID: `0xCAFE`
- Product ID: `0xBABE`

These are development identifiers. Use appropriately assigned USB identifiers for any distributed product or public hardware release; do not assume that the defaults are assigned to you.

## Attribution and license

PicoAudio began as an extension of **[td2sk's `pico-dac2`](https://github.com/td2sk/pico-dac2)**, an original Raspberry Pi Pico USB DAC implementation.

The upstream `LICENSE` file is retained, including its original notice:

```text
Copyright (c) 2025 td2sk
```

Please see [`LICENSE`](LICENSE) for the license text and applicable terms. The original project and its author are credited here; this README does not imply that td2sk authored the PicoAudio extensions.
