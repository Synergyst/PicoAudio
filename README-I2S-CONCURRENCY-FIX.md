# PicoAudio I2S concurrency and slave-underrun fix

This change addresses two hazards in the first two-board master/slave build.

## 1. Slave TX no longer stalls on an empty FIFO

The previous slave PIO transmitter used autopull and could block an `OUT`
instruction when the TX FIFO temporarily emptied. Unlike the master, the
slave does not own BCLK: the master keeps clocking while the slave PIO is
stalled. Missing those edges can shift the slave's outgoing bit stream and
corrupt subsequent channel framing.

The revised `i2s_slave.pio` uses explicit `PULL NOBLOCK` for each 32-bit
channel word. If the FIFO is empty, it loads the zero held in X and transmits
silence for that word. The PIO state machine continues to consume every
external clock edge, preserving bit and channel alignment.

## 2. TX DMA now uses an explicit three-buffer queue

The previous TX path swapped two buffer pointers on every DMA interrupt and
used one boolean to announce that a buffer was ready. If the main loop did not
service that flag between consecutive DMA completions, the pointer swap could
make a buffer writable while it was still queued for DMA.

The revised TX path has three owned buffers with explicit FREE, FILLING, READY,
and DMA states. The main loop reserves, fills, and submits a block. The DMA
interrupt alone selects queued blocks and releases a block only after DMA has
read all of its source words. If no block is queued, DMA streams a permanent
zero block. This avoids unsafe pointer swaps and keeps the master clock
running during brief producer delays.

## Files replaced

- `i2s.c`
- `i2s.h`
- `i2s_slave.pio`
- `audio_device.c`

`audio_device.c` now submits each completed DMA block explicitly. The CMake
configuration from the master/slave role build already generates
`i2s_slave.pio.h`; no CMake change is required.

## Build and flash

From the repository root:

```sh
make build-bridge
```

Then deploy the matching role-specific images using the current role-aware
workflow. The board roles and wiring do not change:

- Pico 2: I2S clock master
- Pico 2 W: I2S clock slave
- GP20 BCLK, GP21 LRCLK, GP22/GP26 crossed DATA, common GND

Do not change the wiring for this software update.

## Validation status

The changes are source-level fixes for identifiable FIFO-stall and DMA-buffer
ownership hazards. The archive has not been compiled against the user's local
Pico SDK or verified on the physical boards in this environment. A successful
`make build-bridge` and repeat of the simultaneous bidirectional capture test
are still required before considering the fix validated.
