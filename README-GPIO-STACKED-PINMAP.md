# GPIO 1:1 Pin Map

The active C diagnostic uses these same-numbered GPIO connections:

- GP16 ↔ GP16: slave-to-master response (slave is the only output)
- GP17 ↔ GP17: chip select (master is the only output)
- GP18 ↔ GP18: clock (master is the only output)
- GP19 ↔ GP19: master-to-slave data (master is the only output)
- GND ↔ GND

No other GPIO or power connection is part of the link. Do not use a fully
connected 40-pin stack header. Use a selective interposer that leaves every
other net disconnected. See [README-GPIO-C-DIAGNOSTIC.md](README-GPIO-C-DIAGNOSTIC.md).
