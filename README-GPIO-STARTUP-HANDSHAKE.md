# PicoAudio GPIO startup interlock

This revision adds a blocking, role-safe GPIO preflight before any audio or USB
UAC2 initialization. It is intended for the bottom Pico 2 master and top Pico 2
W slave connected straight-through on the four GPIO signal lines.

## Fixed GPIO ownership

The GPIO directions are fixed by firmware role and do not change during
startup or normal operation:

| Signal | Bottom Pico 2 master | Top Pico 2 W slave |
|---|---|---|
| GP17 / CS | Output | Input |
| GP18 / SCK | Output | Input |
| GP19 / MOSI | Output | Input |
| GP16 / MISO | Input | Output |
| GND | Common reference | Common reference |

Connect the same GPIO number to the same GPIO number: GP16↔GP16,
GP17↔GP17, GP18↔GP18, GP19↔GP19, plus GND↔GND.

**Only those four GPIO signals and GND are part of this inter-board link.**
Do not connect the other GPIOs, 3V3, 3V3_EN, VSYS, or VBUS between boards.
Keep both boards independently USB-powered.

The link uses RP2350 digital GPIO16–GPIO19 (within GPIO0–GPIO25). The RP2350
Pico 2 datasheet explicitly states that normal digital GPIO0–GPIO25 tolerate
an applied voltage while the RP2350 is unpowered; the ADC-capable GPIO26–GPIO29
have a different restriction. This is why the startup heartbeat is confined
to GP16–GP19. It does not make arbitrary pins or shared power rails safe.

Reference: [Raspberry Pi Pico 2 datasheet, General purpose I/O](https://pip-assets.raspberrypi.com/categories/1005-raspberry-pi-pico-2/documents/RP-008299-DS-1-pico-2-datasheet.pdf)

## What happens at boot

1. Each board initializes only the pins it owns as outputs. The master
   preloads GP17/GP18/GP19 LOW before enabling those outputs. The slave
   preloads GP16 LOW before enabling its output. All opposite-direction pins
   are inputs with pull-downs during preflight.
2. The master emits a 10 ms HIGH probe on GP17, then GP18, then GP19. It probes
   only one output at a time. After each pulse, it waits for the slave's ACK
   on GP16 and for GP16 to return LOW.
3. The slave watches GP17/GP18/GP19. When it detects a probe, it waits for the master to release the probe line,
   then raises its exclusively-owned GP16 output for 12 ms before returning
   GP16 LOW. The master checks for this ACK only after its 10 ms pulse ends.
4. Once all three master outputs have been independently acknowledged, the
   master sends a distinct commit signature: GP17 and GP18 HIGH together,
   with GP19 LOW. The slave acknowledges this on GP16.
5. Only after the commit handshake does the master return GP17 to its normal
   inactive-HIGH chip-select level. Both boards then start the bitbang worker
   and continue into audio initialization.

The master repeats its probes every 250 ms while the slave is absent. The
slave waits for those probes. Either board may be powered first. The first
board remains in preflight instead of starting USB audio prematurely.

## USB/UAC2 startup behavior

`main.c` calls `gpio_audio_link_init()` before `audio_device_init()`,
`audio_capture_init()`, `usb_device_init()`, `usb_audio_init()`, and
`usb_audio_capture_init()`.

Therefore, while its counterpart is absent, a board will **not enumerate its
PicoAudio UAC2 USB device**. After the two boards complete the GPIO handshake,
normal audio and USB initialization runs. Seeing both `cafe:babe` USB devices
is then evidence that both startup handshakes completed, but it is not by
itself proof that PCM is successfully transported.

## Build and flash for the current stack

From `/root/Sources/picoaudio`:

```sh
make clean
make build BOARD=pico2w ROLE=slave
make build BOARD=pico2 ROLE=master
```

Flash the top Pico 2 W slave:

```sh
make deploy BOARD=pico2w ROLE=slave
```

Then flash the bottom Pico 2 master:

```sh
make deploy BOARD=pico2 ROLE=master
```

After flashing, either board may be powered first. The expected sequence is:

- Power only the master: it keeps probing and does not enumerate UAC2.
- Power the slave as well: the two boards acknowledge the GPIO probes, commit,
  and then both enumerate UAC2.
- The same should happen in the reverse power-on order.

## Scope of this change

This change verifies the four GPIO paths before starting UAC2 and prevents the
master and slave firmware from configuring the same link GPIO as opposing
outputs. It does **not** yet establish that the assembly bitbang audio payload
is correct. The last reported capture files were silent, so after this startup
interlock passes, the full-duplex audio test still needs to be run and its PCM
and transport counters checked.
