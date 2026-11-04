# GPIO C Diagnostic — Stage 1

This revision temporarily removes the assembly audio transport. The only
inter-board transport exercised by this build is a deliberately slow,
ordinary-C, full-duplex GPIO challenge/response test. It is intended to prove
pin ownership, edge timing, and data integrity before any PCM transport is
reintroduced.

## Boards and fixed GPIO ownership

- Bottom Raspberry Pi Pico 2: `ROLE=master`
- Top Raspberry Pi Pico 2 W: `ROLE=slave`

Connect **only** these nets between boards:

| Net | Bottom master | Top slave | Direction |
|---|---:|---:|---|
| MISO / ACK | GP16 input | GP16 output | Slave to master |
| CS | GP17 output | GP17 input | Master to slave |
| SCK | GP18 output | GP18 input | Master to slave |
| MOSI | GP19 output | GP19 input | Master to slave |
| Ground | GND | GND | Common reference |

GPIO directions are fixed at initialization and never changed. The master is
the only driver of GP17, GP18, and GP19. The slave is the only driver of GP16.
No handshake or diagnostic phase makes both boards outputs on the same net.

Do not connect the other GPIOs, VBUS, VSYS, 3V3, 3V3_EN, or other header pins
between the boards. A full 40-pin stacking connector is not a valid substitute
for a selectively wired interposer.

## Startup sequence

`main.c` calls `gpio_audio_link_init()` before initializing the audio device or
USB. Both boards therefore remain absent from USB until the link passes.

1. GPIOs are initialized with the fixed directions shown above.
2. The master pulses GP17, GP18, and GP19 individually for 10 ms.
3. The slave waits for each pulse to return LOW and acknowledges on GP16 for
   12 ms. The master does not change any direction to receive the ACK.
4. The master sends the two-line commit signature on GP17 + GP18 and waits for
   the slave's acknowledgement.
5. The master performs 64 challenge rounds using slow C bit-banging. Each
   round carries a changing challenge, a derived token, and a slave status.
6. The slave independently checks the received token. A final two-step commit
   ensures the slave only passes after the master has received the slave's
   response and sent the final acknowledgement.
7. Only after both sides have passed does `gpio_audio_link_init()` return and
   allow UAC2 initialization to proceed.

The C transfer is SPI mode 0: MOSI is set while SCK is LOW, the slave samples
MOSI on the rising edge, and the master samples MISO while SCK is HIGH. The
slave advances MISO after the falling edge. The diagnostic clock is about
28 kbit/s; this is intentionally nowhere near audio throughput.

## What this stage does and does not do

**It verifies:**

- GP17 chip-select reaches the slave.
- GP18 clock edges reach the slave.
- GP19 data reaches the slave correctly.
- GP16 response data reaches the master correctly.
- Both boards agree on the 64-round test and final commit.
- UAC2 is not initialized while the peer is missing or the test fails.

**It does not transport PCM audio.** The existing UAC2 descriptors and USB
playback/capture plumbing are retained so the rest of the application remains
available for later stages, but the GPIO audio API functions are temporary
compatibility stubs: playback samples are discarded and capture returns no
frames. Silence from the UAC2 capture endpoints is therefore expected in this
Stage-1 build. Do not run the old `test-gpio-bitbang.sh` audio loopback test as
a pass/fail test for this stage.

The successful test is reported on each board's UART log as:

- Master: `GPIO C diagnostic PASSED: 64 rounds, master->slave and slave->master`
- Slave: `GPIO C diagnostic PASSED: slave received all challenge tokens and final commit`

Since USB initialization happens after the test, seeing both `cafe:babe`
devices enumerate is also evidence that both boards reached the post-test
startup path. It is not a substitute for retaining the UART diagnostic log.

## Build and deploy

From `/root/Sources/picoaudio`:

```bash
make clean
make build BOARD=pico2w ROLE=slave
make build BOARD=pico2 ROLE=master
make deploy BOARD=pico2w ROLE=slave
make deploy BOARD=pico2 ROLE=master
```

Flash the top Pico 2 W as slave and the bottom Pico 2 as master. Both boards
may be powered in either order. The board that boots first waits with USB audio
uninitialized until the peer completes the handshake and diagnostic.

## Host-side check

After both boards start, run:

```bash
./tools/check-gpio-c-diagnostic.sh
```

The check expects exactly two `cafe:babe` USB devices. The diagnostic itself
runs automatically before enumeration.

## PIO and assembly status

There is no PIO state machine or ARM assembly in the inter-board transport in
this build. `gpio_audio_link_bitbang.S` is no longer listed in CMake and is an
inert placeholder. The project still uses the LED PIO program on the standard
Pico 2; that PIO code is unrelated to the inter-board link.

## Next stage

Only after this test passes consistently in both power-up orders should a
small C data transport be introduced. PCM framing, buffering, and performance
optimization remain separate later stages.
