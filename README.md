# PicoAudio

**USB Audio Class 2.0 full-duplex audio firmware for Raspberry Pi Pico boards, using PIO/DMA-based I²S playback and capture.**

PicoAudio presents each Pico as a stereo USB audio device with simultaneous playback and capture. USB PCM is moved to and from digital I²S using the RP2040/RP2350 PIO and DMA hardware; this path does not use an analog ADC or DAC.

## Current project status

- USB playback and capture have been tested concurrently on one Pico.
- A single-board I²S TX-to-RX loopback has been tested on a Pico 2 W.
- Separate I²S clock-master and clock-slave firmware roles are now implemented in the source tree.
- The two-board wiring and data path still require hardware validation. Do not treat the two-board bridge as verified until the staged tests in [README-DUAL-BOARD-ROLES.md](README-DUAL-BOARD-ROLES.md) pass.

## Features

- USB Audio Class 2.0 (UAC2) stereo playback and capture.
- Simultaneous USB playback and capture on each board.
- 44.1 kHz and 48 kHz sample rates.
- Playback formats: S16_LE, packed S24_3LE, and S32_LE.
- Capture format: stereo S32_LE.
- 32-bit I²S slots for the full-duplex data path.
- PIO/DMA-based I²S transmit and receive.
- Asynchronous USB playback with an explicit feedback endpoint.
- Compile-time I²S clock-master and clock-slave roles.
- RP2040 and RP2350 build configurations.

## Supported boards

| Board | MCU | Pico SDK board name | Role / build status |
|---|---|---|---|
| Raspberry Pi Pico | RP2040 | `pico` | Master build retained; previously build-tested |
| Raspberry Pi Pico 2 | RP2350 | `pico2` | New clock-slave target; compile and hardware validation required on this board |
| Raspberry Pi Pico 2 W | RP2350 | `pico2_w` | Clock-master target; previously build- and hardware-tested in single-board mode |

PicoAudio does not use Wi-Fi. The Pico 2 W onboard LED is connected through the CYW43 chip, so this build does not use it as a normal PIO GPIO LED. The non-W Pico 2 uses the standard `pico2` board definition.

## I²S pin assignment

| Signal | GPIO | Master board | Slave board |
|---|---:|---|---|
| BCLK | GP20 | Output | Input |
| LRCLK | GP21 | Output | Input |
| TX DATA | GP22 | Output | Output |
| RX DATA | GP26 | Input | Input |

The master is the only board that drives BCLK and LRCLK. The slave transmitter is synchronized to those external clocks and only drives its own DATA pin.

## Build the two-board firmware pair

Set `PICO_SDK_PATH` to your Pico SDK 2.2.0 checkout, then run this from the repository root:

```sh
make build-bridge
```

This builds two separate RP2350 images:

| Physical board | Firmware role | Build output |
|---|---|---|
| Pico 2 W | I²S clock master | `build-pico2w/mdac_adc2.uf2` |
| Pico 2 (non-W) | I²S clock slave | `build-pico2-slave/mdac_adc2.uf2` |

The images are not interchangeable: flash the master image to the Pico 2 W and the slave image to the non-W Pico 2. Use the role-aware `make deploy` helper to select the board and role. It recognizes Pico 1 from its `RPI-RP2` BOOTSEL label; for RP2350 devices it asks whether the physical board is Pico 2 or Pico 2 W because both expose the same `RP2350` label.

Select any board and role when building:

```sh
make build BOARD=pico1 ROLE=master
make build BOARD=pico2 ROLE=slave
make build BOARD=pico2w ROLE=master
```

Convenience targets are also available: `make build-pico1`, `make build-pico2`, and `make build-pico2w` (each defaults to master; use `ROLE=slave` to change it). With no `BOARD` specified, `make build` builds the standard Pico 2 W master + Pico 2 slave bridge pair.

Deployment detects the RP2040 Pico 1 automatically. Since the Pico 2 and Pico 2 W BOOTSEL volumes both identify as `RP2350`, the helper asks which model is present and which role to flash:

```sh
make deploy
make deploy BOARD=pico2w ROLE=master
make deploy BOARD=pico2 ROLE=slave
```

Run deployment as root. Each invocation builds and flashes one selected BOOTSEL board. See [README-DUAL-BOARD-DEPLOY.md](README-DUAL-BOARD-DEPLOY.md) for details.

## Two-board wiring

After flashing the matching role-specific images, wire the boards as follows:

```text
Pico 2 W (MASTER)                  Pico 2 (SLAVE)

GP20  BCLK OUT  -----------------> GP20  BCLK IN
GP21  LRCLK OUT -----------------> GP21  LRCLK IN
GP22  TX DATA    ----------------> GP26  RX DATA
GP26  RX DATA    <---------------- GP22  TX DATA
GND              ----------------- GND
```

The two DATA connections are crossed. Do not connect GP22 to GP22 or GP26 to GP26. Connect the grounds, but keep the boards independently USB-powered. **Do not connect VBUS, VSYS, or 3V3 between the boards.**

The master must be actively streaming USB playback to generate BCLK/LRCLK. If master playback stops, the shared I²S clocks stop, so slave transmission and both capture paths stop receiving new samples. Configure the two boards' USB streams for the same nominal sample rate (44.1 kHz or 48 kHz).

## USB format and bandwidth notes

The USB interface is full-speed. The current maximum isochronous packet buffer is 392 bytes (49 stereo frames when each frame is two 32-bit samples). The 24-bit playback alternate is packed three-byte `S24_3LE`; the firmware converts playback samples into the 32-bit words used by the I²S data path. Only 44.1 kHz and 48 kHz are enabled in the current full-speed profile.

## Linux verification

List the audio devices:

```sh
aplay -l
arecord -l
```

For one-board loopback, use `speaker-test` for playback and `arecord` for capture on the same PicoAudio card. For the two-board test procedure and exact direction-by-direction checks, see [README-DUAL-BOARD-ROLES.md](README-DUAL-BOARD-ROLES.md).

## Repository layout

| Path | Purpose |
|---|---|
| `main.c` | Firmware entry point; logs the selected I²S role |
| `audio_device.c/.h` | USB playback buffering and clock handling |
| `audio_capture.c/.h` | Capture-side audio handling |
| `i2s.c`, `i2s.pio` | I²S transmit; existing clock-master implementation |
| `i2s_slave.pio` | New external-clock I²S slave transmitter |
| `i2s_rx.c/.h`, `i2s_rx.pio` | I²S receive implementation |
| `usb_audio.c` | USB playback handling and sample conversion |
| `usb_audio_capture.c/.h` | USB capture streaming |
| `usb_descriptor.h` | Composite USB audio/HID descriptors |
| `Makefile` | Board- and role-specific build targets |

## Attribution and license

PicoAudio began as an extension of **[td2sk's `pico-dac2`](https://github.com/td2sk/pico-dac2)**. The original `LICENSE` file is retained, including its original copyright notice, `Copyright (c) 2025 td2sk`. See [LICENSE](LICENSE) for the complete license terms. The PicoAudio extensions are not attributed to the original author.
