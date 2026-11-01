# PicoAudio: two-board I²S master/slave setup

This document describes the role-specific firmware and the first hardware tests for connecting two Pico boards.

> **Hardware validation status:** Both role-specific firmware variants have compiled successfully. The two-board link has not yet been electrically tested. Do not treat the bridge as verified until both directional tests below pass.

## Board roles

| Board | Role | BCLK GP20 | LRCLK GP21 | TX DATA GP22 | RX DATA GP26 |
|---|---|---|---|---|---|
| Pico 2 W | Master | Output | Output | Output | Input |
| Pico 2 (non-W) | Slave | Input | Input | Output | Input |

The master runs the existing side-set PIO transmitter, which generates BCLK and LRCLK. The slave runs `i2s_stereo_slave` from `i2s_slave.pio`; it waits for the master's clock edges and changes its DATA output while BCLK is low. The slave PIO program never configures GP20 or GP21 as outputs.

Both boards run the I²S receive PIO on their local GP26 input. The master receives the slave's TX DATA, while the slave receives the master's TX DATA.

## 1. Build and flash each board

The Makefile supports all three board models and both roles.

Build the intended Pico 2 W master + Pico 2 slave pair:

```sh
make build-bridge
```

Or select any model/role combination individually:

```sh
make build BOARD=pico1 ROLE=master
make build BOARD=pico1 ROLE=slave
make build BOARD=pico2 ROLE=master
make build BOARD=pico2 ROLE=slave
make build BOARD=pico2w ROLE=master
make build BOARD=pico2w ROLE=slave
```

For interactive deployment, run:

```sh
make deploy
```

The helper detects Pico 1 automatically from its `RPI-RP2` BOOTSEL label. For an `RP2350` BOOTSEL volume, it asks whether the physical board is a Pico 2 or Pico 2 W, because both share that label. It then asks whether to flash master or slave firmware. You can bypass the prompts with explicit values:

```sh
make deploy BOARD=pico2w ROLE=master
make deploy BOARD=pico2 ROLE=slave
```

Run deployment as root. Each invocation builds and flashes exactly one selected BOOTSEL device. To avoid confusing two RP2350 boards with identical boot labels, put only the intended board into BOOTSEL for that invocation.

## 2. Wiring

Connect these signals only after the correct role images have been flashed:

```text
Pico 2 W (MASTER)                  Pico 2 (SLAVE)

GP20  BCLK OUT  -----------------> GP20  BCLK IN
GP21  LRCLK OUT -----------------> GP21  LRCLK IN
GP22  TX DATA    ----------------> GP26  RX DATA
GP26  RX DATA    <---------------- GP22  TX DATA
GND              ----------------- GND
```

The data lines are crossed:

- Master GP22 → Slave GP26
- Slave GP22 → Master GP26

Do not connect GP22-to-GP22 or GP26-to-GP26. Do not connect either board's VBUS, VSYS, or 3V3 to the other board. Each Pico should receive power from its own USB connection. Connect the grounds so the digital signals have a common reference.

## 3. Clock and sample-rate behavior

The master generates I²S clocks only while its USB playback stream is active and its playback engine has started. The slave cannot generate fallback clocks. If master playback stops, BCLK/LRCLK stop; the slave transmitter waits, and both capture paths stop receiving new frames.

Configure both boards' playback and capture streams for the same nominal rate (44.1 kHz or 48 kHz). The slave's PIO transmitter follows the physical master clock, not its local clock divider. Matching the host-selected rates is necessary for the current independent USB clock-control implementation.

## 4. Test one direction at a time

Connect both boards to the host and identify which ALSA card belongs to each Pico using `aplay -l` and `arecord -l`. Substitute the correct card numbers below.

### Test A: master → slave

Start S32_LE playback on the Pico 2 W and record capture from the non-W Pico 2:

```sh
speaker-test -D hw:MASTER,0 -c 2 -r 48000 -F S32_LE -t sine -f 1000
```

In another terminal:

```sh
arecord -D hw:SLAVE,0 -f S32_LE -c 2 -r 48000 -d 10 /tmp/picoaudio-master-to-slave.wav
```

The slave capture should contain the master's test tone.

### Test B: slave → master

Keep master playback active so it continues generating BCLK/LRCLK. Start S32_LE playback on the slave and record capture from the master:

```sh
speaker-test -D hw:SLAVE,0 -c 2 -r 48000 -F S32_LE -t sine -f 1500
```

In another terminal:

```sh
arecord -D hw:MASTER,0 -f S32_LE -c 2 -r 48000 -d 10 /tmp/picoaudio-slave-to-master.wav
```

The master capture should contain the slave's test tone. The master playback stream must remain active throughout this test because it supplies the shared clocks.

Stop each `speaker-test` with Ctrl+C after its corresponding recording finishes.

## 5. What to report if a test fails

Please retain the complete build output and report:

- Whether `make build-bridge` completed successfully.
- Which board and role were flashed onto each physical Pico.
- UART startup role messages from both boards.
- `aplay -l` and `arecord -l` output showing both devices.
- Whether Test A and Test B captured the expected tone, including silence, channel swaps, noise, or dropouts.

Never connect two boards' BCLK/LRCLK pins while both are running master firmware. Only the master is allowed to drive the shared clock wires.
