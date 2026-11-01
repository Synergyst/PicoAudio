# Full-speed USB duplex bandwidth profile

This change set sizes both isochronous audio endpoints for simultaneous USB
full-speed playback and capture. The maximum is 49 stereo frames per 1 ms
packet: 49 * 2 channels * 4 bytes = 392 bytes. The extra frame allows the
asynchronous packet scheduler to accommodate one frame of clock variation.

The shared UAC2 clock now advertises only 44.1 kHz and 48 kHz, and the audio
clock setter rejects 88.2/96 kHz. At full speed, advertising 97-frame packets
(776 bytes) on both playback and capture endpoints reserves 1552 bytes per
frame before protocol overhead, which the host cannot schedule.

Playback remains available in 16-, 24-, and 32-bit formats. Capture remains
stereo S32_LE. The endpoint packet buffers, playback OUT re-arm length, and
capture packet frame limit are kept consistent at 392 bytes / 49 frames.

## Apply

Extract this archive into the project root, preserving the file paths, then
rebuild the existing RP2350 build directory. Do not reconfigure the board or
change the current RP2350 CMake settings.

This is a full-speed profile. 88.2/96 kHz simultaneous duplex requires a
higher-bandwidth USB transport; those rates are intentionally not advertised
by this firmware profile.
