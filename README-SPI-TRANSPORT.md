# PicoAudio SPI transport

This replacement removes I2S from the active firmware build. Both boards remain
USB UAC2 playback/capture devices; SPI0 carries the two 32-bit stereo PCM
streams in full duplex.

## Wiring

Use SPI0 on GP16-GP19. Connect the same GPIO numbers as shown below; MOSI/MISO
are crossed because each board's TX connects to the other board's RX.

| Signal | Master Pico 2 W | Slave Pico 2 |
|---|---|---|
| SPI0 SCK | GP18 (physical pin 24) | GP18 (physical pin 24) |
| SPI0 CSn | GP17 (physical pin 22) | GP17 (physical pin 22) |
| Master MOSI / TX | GP19 (physical pin 25) | GP16 (physical pin 21) |
| Slave MISO / TX | GP16 (physical pin 21) | GP19 (physical pin 25) |
| Ground | GND (physical pin 23) | GND (physical pin 23) |

There are four signal wires plus ground. Do not connect VBUS, VSYS, or 3V3
between the boards. The master drives CSn as a GPIO; the slave uses GP17 as the
SPI0 hardware chip-select input.

## Transport details

- SPI0, mode 0, 8-bit transfers, 7.5 MHz.
- Master initiates one fixed-size full-duplex transaction every 4 ms.
- Each direction carries up to 192 stereo frames per packet, using signed
  32-bit samples. At 48 kHz this is 4 ms of audio; 44.1 kHz uses a fractional
  176/177-frame pattern.
- Each packet has a magic value, sequence number, source sample rate, frame
  count, and CRC32. Invalid packets are discarded.
- DMA moves the SPI data. The main loop continues servicing USB while a packet
  is in flight.
- Playback is buffered before transport. Capture has an 8 ms pre-roll and a
  fixed-point linear resampler, so the two boards can independently run at
  44.1 kHz or 48 kHz. The PCM word format remains S32 stereo; only the sample
  positions are interpolated when nominal rates differ.
- At 48 kHz, the payload is 384,000 bytes/s in each direction. SPI is full
  duplex, so both streams travel simultaneously.

## Build and flash

The command interface is unchanged:

```sh
make build BOARD=pico2w ROLE=master
make build BOARD=pico2 ROLE=slave

make deploy BOARD=pico2w ROLE=master
make deploy BOARD=pico2 ROLE=slave
```

Use separate build directories as the Makefile does. The firmware now uses the
`PICODAC_SPI_ROLE` CMake setting; the old I2S role and RX pin settings are no
longer used by the build.

## First test

1. Rewire the boards to the SPI0 table above. Keep the common ground.
2. Flash the master image to the Pico 2 W and the slave image to the Pico 2.
3. Confirm both devices enumerate over USB.
4. Start playback on both devices and capture from the opposite device at the
   same time. Start with 48 kHz, S32_LE, stereo.
5. Repeat at 44.1 kHz. Then test one board at 44.1 kHz and the other at 48 kHz;
   the capture resampler should keep the audio running, with additional latency.

The archive replaces complete project files at their existing relative paths.
The old I2S source files may remain in the working tree, but CMake no longer
compiles or generates PIO code for them. They can be deleted after the SPI
transport has been accepted:

```sh
rm -f i2s.c i2s.h i2s.pio i2s_rx.c i2s_rx.h i2s_rx.pio i2s_slave.pio
```

This is a first SPI transport implementation, not a claim of hardware
verification. Check the build on the CM5 before flashing, then run simultaneous
bidirectional playback/capture and inspect both recordings for packet loss,
underruns, and channel integrity.
