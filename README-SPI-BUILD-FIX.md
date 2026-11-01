# PicoAudio SPI build fix 1

This archive contains a complete replacement for `spi_link.c` from the initial
SPI transport archive.

The slave-only DMA queue/task code calls the Pico SDK interrupt-state helpers
`save_and_disable_interrupts()` and `restore_interrupts()`. Those functions are
declared by `hardware/sync.h`; the initial archive omitted that include. This
replacement adds the missing SDK header and otherwise preserves the SPI
transport implementation.

From `/root/Sources/picoaudio`, extract this archive and rebuild both targets:

```sh
unzip -o /tmp/PicoAudio-spi-transport-fix1.zip -d /root/Sources/picoaudio
make build BOARD=pico2w ROLE=master
make build BOARD=pico2 ROLE=slave
```

Do not flash either image until both builds complete successfully.
