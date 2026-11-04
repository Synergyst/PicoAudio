# GPIO Startup Handshake

See [README-GPIO-C-DIAGNOSTIC.md](README-GPIO-C-DIAGNOSTIC.md) for the current
implementation. The startup preflight and the full-duplex C diagnostic both
run before audio and USB initialization. GPIO ownership is fixed: master
outputs GP17/GP18/GP19, slave output GP16, with the other side always input.
