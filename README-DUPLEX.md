# PicoAudio: USB playback + digital I²S capture

This firmware keeps the original USB DAC playback path and adds a UAC2 stereo capture interface. Each board can expose playback and capture simultaneously. Capture is digital I²S PCM; no analog ADC is used.

## USB audio interfaces

- Playback: stereo, S16_LE, packed S24_3LE, or S32_LE.
- Capture: stereo S32_LE.
- Sample rates: 44.1 kHz and 48 kHz.
- Full-speed USB packet sizing is limited to the configured maximum of 392 bytes.
- Playback USB formats are converted to 32-bit words for the I²S data path.

## I²S pins

| Signal | GPIO | Master | Slave |
|---|---:|---|---|
| BCLK | GP20 | Output | Input |
| LRCLK | GP21 | Output | Input |
| TX DATA | GP22 | Output | Output |
| RX DATA | GP26 | Input | Input |

The master uses the existing clock-generating TX PIO program. The slave uses the new `i2s_stereo_slave` PIO program, which waits for external BCLK/LRCLK and changes DATA on BCLK falling edges. The slave state machine never drives the clock pins.

## Build

```sh
make build-bridge
```

Outputs:

- `build-pico2w/mdac_adc2.uf2`: Pico 2 W, clock master.
- `build-pico2-slave/mdac_adc2.uf2`: non-W Pico 2, clock slave.

Flash each UF2 to its matching physical board. Do not use the automatic deployment helper to choose between these two RP2350 variants: both BOOTSEL volumes are labelled `RP2350`, so the helper cannot reliably tell them apart.

## One-board loopback

For a single board running the master image, connect:

```text
GP22 (I²S TX DATA)  ----  GP26 (I²S RX DATA)
```

The local master PIO generates BCLK on GP20 and LRCLK on GP21. Start S32_LE playback and capture simultaneously. The verified earlier single-board setup used a Pico 2 W.

## Two-board operation

See [README-DUAL-BOARD-ROLES.md](README-DUAL-BOARD-ROLES.md) for the complete wiring, safe power arrangement, and staged tests. The role-specific transmitter has been implemented, but the interconnected two-board data path still needs hardware validation.
