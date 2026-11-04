# GPIO Transport Status

The previous assembly-based, packetized PCM transport is retired from the
active build. The current Stage-1 implementation is an ordinary-C GPIO
challenge/response diagnostic only. It does not forward PCM audio.

See [README-GPIO-C-DIAGNOSTIC.md](README-GPIO-C-DIAGNOSTIC.md) for pin
ownership, handshake protocol, build instructions, and the next development
stage. PIO is used only for the separate status LED program, not the
inter-board signal transfer.
