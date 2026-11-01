# PicoAudio I2S concurrency and clock-lifecycle fix

This update preserves the existing triple-buffer TX ownership fix and changes
when the physical I2S clock generator runs.

## 1. Master clocks are independent of USB playback

The master now initializes its I2S transmitter as a 32-bit stereo-slot engine
and starts the PIO/DMA clock source during `audio_device_init()`. It remains
active while USB playback is stopped or has not yet started. When there are no
queued audio blocks, DMA repeatedly supplies a permanent zero buffer, so the
master continues generating BCLK and LRCLK without transmitting stale audio.

Ordinary USB playback start/stop no longer deinitializes or reinitializes the
master I2S hardware. Starting playback releases the DMA silence override and
allows submitted audio blocks to be consumed. Stopping playback discards
queued/filling application buffers and lets the current DMA block finish;
the next DMA interrupt selects silence. The PIO clock generator is not
stopped.

The master starts with 32-bit I2S slots because USB playback formats are
normalized to signed 32-bit samples before entering the I2S transmit path.
This keeps the clock rate and framing consistent from power-on, before a USB
playback application opens the device.

## 2. Stream restart cleanup

Playback stream stop now flushes the application ring buffer. Stream start
also clears it after resizing, and any final USB OUT packet delivered after
the interface has stopped is ignored. This prevents samples left over from a
previous USB session from being replayed when a later session starts.

## 3. Sample-rate changes

Changing between the supported 44.1 kHz and 48 kHz rates requires changing the
master PIO clock divider. That operation intentionally deinitializes and
reinitializes I2S once at the new rate. Master `i2s_init()` immediately
restarts the clock generator afterward. This is distinct from ordinary USB
playback open/close, which must not interrupt the clock link.

## 4. TX buffer ownership

The TX path continues to use three application buffers with explicit FREE,
FILLING, READY, and DMA states. The main loop reserves, fills, and submits a
block. The DMA interrupt releases a block only after DMA has consumed it and
selects the next queued block. If no audio block is ready, DMA streams the
permanent zero buffer.

The slave transmitter retains its startup synchronization to external
LRCLK/BCLK and `PULL NOBLOCK`, so an empty TX FIFO transmits zero rather than
stalling on the master's clocks. The slave PIO state machine is restarted from
its program entry when its local USB playback stream is restarted, allowing it
to synchronize to the continuously-running master link.

## Files replaced

- `i2s.c`
- `audio_device.c`
- `README-I2S-CONCURRENCY-FIX.md`

No pin assignments, USB descriptors, sample formats, or board roles change.

## Build and deploy

Build each Pico 2 W role separately:

```sh
make build BOARD=pico2w ROLE=master
make build BOARD=pico2w ROLE=slave
```

Deploy one board at a time while it is in BOOTSEL:

```sh
make deploy BOARD=pico2w ROLE=master
make deploy BOARD=pico2w ROLE=slave
```

Keep the established wiring unchanged:

- Master GP20 BCLK OUT -> Slave GP20 BCLK IN
- Master GP21 LRCLK OUT -> Slave GP21 LRCLK IN
- Master GP22 TX DATA -> Slave GP26 RX DATA
- Slave GP22 TX DATA -> Master GP26 RX DATA
- GND -> GND

Do not connect the boards' VBUS, VSYS, or 3V3 rails together.

## Validation

This is a source update intended to remove ordinary USB playback start/stop
from the I2S clock lifecycle and prevent old playback samples from carrying
across USB sessions. It still needs to be built with the project's local Pico
SDK, flashed to both physical boards, and verified with repeated and longer
simultaneous playback/capture tests. A successful build alone does not
establish audio stability.
