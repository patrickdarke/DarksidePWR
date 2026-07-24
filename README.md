# Darkside PWR

Victron power monitor in the DSODash design language, for the ELECROW
CrowPanel Advance 3.5" (ESP32-S3, 480×320 ILI9488 SPI, GT911 touch).
Polls a Victron GX device (Ekrano) over **Modbus TCP** on the local
network — no BLE keys, no cloud, works anywhere the truck is.

## Data path

`gx_modbus.cpp` reads unit **100** (`com.victronenergy.system`) on port 502,
registers verified live against the Darkside.Overland Ekrano (2026-07-24):

| Register | Meaning | Scale |
|---|---|---|
| 840 | Battery voltage | ÷10 V |
| 841 | Battery current (signed) | ÷10 A |
| 842 | Battery power (signed) | W |
| 843 | State of charge | % |
| 844 | Battery state | 0 idle · 1 charging · 2 discharging |
| 850 | PV (DC-coupled) power | W |
| 817 | AC consumption L1 | W |
| 860 | DC system power | W |

GX prerequisites (Settings → Integrations): **Modbus TCP Server = Enabled**.
Addressing: `venus.local` via mDNS, falling back to the pinned IP in
`secrets.h` (keep a DHCP reservation for the GX).

## Building

One-time: `cp secrets.h.example secrets.h` and fill in Wi-Fi + GX address.

```
./build.sh          # compile
./build.sh flash    # compile + flash the attached usbmodem port
```

`lib/` pins the display stack at the versions ELECROW ships with the panel
(LVGL 8.3.11 + LovyanGFX 1.1.16 + their `lv_conf.h`); `LovyanGFX_Driver.h`
is their lesson-03 panel config for board revisions V1.2–V1.4, unmodified.
Vendor source: github.com/Elecrow-RD/CrowPanel-Advance-3.5-HMI-ESP32-S3-AI-Powered-IPS-Touch-Screen-480x320

## Status

Skeleton: Wi-Fi + Modbus poll at 1 Hz + the main power screen (SOC arc,
house V, battery A, solar W, AC W, DC/net footer, link dot). Touch is wired
but unused. Ideas next: tanks/temps second screen (more Modbus units),
alternator tile (Orion XS), low-SOC alert, LEDC backlight dimming.
