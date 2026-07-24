# Darkside PWR — project laws and hard-won facts

Victron power monitor for the truck ("Darkside.Overland" system) on the
ELECROW CrowPanel Advance 3.5". Sibling of the DSODash project and follows
its design language (dark `0x0C1018` bg, tiles `0x111826`, teal/green/amber/
blue accents, montserrat) and its working rules. Read this file before
touching anything — every fact here was verified on hardware or against the
live GX, and several cost real debugging time.

## Machine setup (new computer)

```
brew install arduino-cli gh
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.10     # PINNED — see "Toolchain" below
gh auth login                                   # repo is private
git clone https://github.com/patrickdarke/DarksidePWR
cd DarksidePWR
cp secrets.h.example secrets.h                  # then fill in Wi-Fi SSID/pass
./build.sh                                      # compile
./build.sh flash                                # compile + flash attached panel
```

`secrets.h` is gitignored and holds the ONLY copy of real Wi-Fi credentials —
NEVER put them in a tracked file (this repo's history was verified clean
before first push; keep it that way). All GX addressing/unit-id config also
lives there; the committed `secrets.h.example` carries correct defaults for
everything except the Wi-Fi credentials.

## Hardware (CrowPanel Advance 3.5", board rev V1.2–V1.4)

- ESP32-S3-WROOM-1-N16R8: 16 MB flash, 8 MB **octal** PSRAM (`PSRAM=opi`),
  native USB CDC — enumerates as `/dev/cu.usbmodem*`, no UART bridge.
- Panel: ILI9488, 40 MHz SPI — SCLK 42, MOSI 39, DC 41, CS 40, RST 2;
  `offset_rotation=3` → 480×320 landscape; `invert=true`.
- Touch: GT911 I²C — SDA 15, SCL 16, INT 47, RST 48, addr 0x14 (wired into
  LVGL, currently unused).
- Backlight: plain GPIO 38 HIGH (no PWM wired in firmware yet; LEDC possible).
- Expansion headers: J13 = I²C (IO15/16, shared with touch), J15 = UART1
  (IO17/18). Board has TF slot, mic, speaker (pins unverified).
- `LovyanGFX_Driver.h` is ELECROW lesson-03 config, unmodified. Vendor repo:
  `Elecrow-RD/CrowPanel-Advance-3.5-HMI-ESP32-S3-AI-Powered-IPS-Touch-Screen-480x320`
  (revs V1.0 vs V1.2-1.4 have separate example trees).

### Flashing + serial rules (cost a false "black screen" alarm)

- First flash over FACTORY firmware fails the auto-reset ("No serial data
  received") → hold BOOT, tap RST, retry. Once THIS firmware (CDCOnBoot=cdc)
  is on, `./build.sh flash` resets work normally.
- Serial monitor: open the usbmodem port with pyserial **defaults** (DTR/RTS
  asserted). Setting `dtr=False/rts=False` BEFORE open straps the S3 into ROM
  download mode — the screen goes black because the chip is parked in the
  bootloader, not because anything crashed. `script`-wrapped arduino-cli
  monitor does not work (tcgetattr on socket), and killing `arduino-cli
  monitor` loses its buffered output.
- Telemetry: one `[gx] ...` line per 1 Hz poll carries every displayed value —
  verify changes by telemetry, not by eyeball.

## Toolchain (pinned, vendored)

- esp32 core **3.3.10** (same as DSOdash fleet — do not drift casually).
- `lib/` vendors the display stack; `--libraries ./lib` in build.sh:
  - LVGL **8.3.11** — ELECROW's exact copy + their `lv_conf.h` (montserrat
    12/14/20/28/48 enabled; LVGL v8 API: draw_buf/disp_drv, label recolor).
  - LovyanGFX **1.2.26** — DELIBERATE upgrade from vendor's 1.1.16, which
    does not compile against core 3.3.10/IDF 5.5 (i2c_periph_signal.module,
    lcd_periph_signals errors). Same config API.
  - LovyanGFX's CJK font dirs (`src/lgfx/Fonts/IPA`, `Fonts/efont`) are
    hard-`#include`d by `lgfx_fonts.cpp` — they CANNOT be trimmed.
- FQBN: `esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB`.
- Dual full-frame LVGL buffers in PSRAM (vendor lesson pattern).

## The Victron system (data source)

Ekrano GX v3.75, `venus.local` / fallback `10.20.30.239` (DHCP reservation),
VRM installation "Darkside.Overland" (714970), portal id `c0619ab7d41e`.
Devices: MultiPlus 12/2000/80-50, Lynx Smart BMS (battery monitor — there is
NO SmartShunt), SmartSolar MPPT (ext. control), Orion XS alternator charger,
3 Mopeka LPG tanks, 3 Ruuvi temperature sensors. Venus OS Large (Node-RED on
1881 https). mDNS note: `venus.local` resolves on the truck SSID (where the
display lives), but NOT across routed segments (a bench Mac on another
subnet must use the IP).

### Why Modbus TCP (and not MQTT/BLE)

- **Modbus TCP (port 502): enabled, no auth — THE data path.** Verified live.
- MQTT: "MQTT Access" is on, but Venus 3.75 requires authenticated clients
  (8883 → CONNACK rc=5; no plaintext 1883 listener exists in this firmware).
  Old anonymous local MQTT is gone. Don't chase it without a reason.
- BLE instant-readout: rejected — misses VE.Bus/tanks/temps and needs
  per-device AES keys; the GX already aggregates everything.

### Register map (ALL verified against the live system 2026-07-24)

Authoritative source: `victronenergy/dbus_modbustcp` → `attributes.csv`.
On Venus 3.75 the **Modbus unit id = device instance** (the number VRM shows
in brackets, e.g. "Temperature sensor [22]"); the legacy `unitid2di.csv`
mapping does NOT cover dynamically-numbered devices. One unit serves every
service sharing that instance (unit 23 = Fridge temp AND Blantons tank —
address range selects the service). To find a device: sweep units 1–247
reading a service-specific register (e.g. 3304 for temps, 4100 for
alternator, 3004 for tanks).

| Unit | Service | Reg | Meaning | Scale |
|---|---|---|---|---|
| 100 | system | 840/841/842 | battery V / A(signed) / W(signed) | ÷10 / ÷10 / 1 |
| 100 | system | 843/844 | SOC % / state (0 idle, 1 chg, 2 dis) | 1 |
| 100 | system | 850 | PV power W | 1 |
| 100 | system | 817 | AC consumption L1 W | 1 |
| 100 | system | 860 | DC system W | 1 |
| 239 | alternator (Orion XS) | 4100/4101 | out V / out A(signed) — power = V×A, no power reg | ÷100 / ÷10 |
| 239 | alternator | 4102/4112 | input(starter) V ÷10 (0 when idle) / charge state | |
| 22/23/25 | temperature (Ruuvi Out/Fridge/In) | 3304 | °C ×100 (3306 humidity ×10, 3308 hPa unused) | ÷100 |
| 23/24/25 | tank (Blanton/Elijah/Pappy) | 3004 | level % | ÷10 |

### Reliability laws

- System reads (unit 100) are FATAL on failure → drop socket, 3 s backoff,
  re-resolve on reconnect. Sensor reads (alternator/temps/tanks) are
  **per-device NON-FATAL**: a dead/napping device shows `--` (or 0 W for the
  alternator), never stale numbers, never a failed poll. A month-dead Mopeka
  ("Elijah Craig") drops off D-Bus entirely — its unit stops answering; this
  is normal and self-heals when the sensor returns.
- `readRegs` parses the MBAP length before the body so Modbus exceptions are
  consumed cleanly (no socket desync, no 500 ms stall).

## UI map (ui.cpp)

SOC arc (teal) + big % + CHARGING/DISCHARGING/IDLE · tiles: HOUSE V /
CURRENT A (amber) / POWER IN W (green, = PV + alternator) / AC LOADS W
(blue) · footer: temps line (OUT/FRIDGE/IN °F), tanks line (red below
`kTankLowPct = 20`%), power line (PV · ALT · DC · NET). Link dot: red = no
Wi-Fi, amber = Wi-Fi but no GX, green = live (10 s staleness window).
Labels/units are config in `secrets.h`, threshold in `ui.cpp`.

## Parked / next ideas

- **Battery gauge for the display itself — needs HARDWARE** (schematic-proven
  on V1.4): TP4059 charges autonomously; its CHRG line goes only to the
  isolated STC8G1K08 (UART pads → J14 header, not connected to the S3);
  VBAT has NO sense divider. Options when resumed: (a) VBAT pad P9 → 300k →
  IO18 (J15) → 100k → GND, read on ADC2 with Wi-Fi-contention retries at slow
  cadence; (b) MAX17048 fuel gauge on the J13 I²C port (better: real %, rate,
  charge detection).
- Second page on touch (GT911 already wired): humidity/pressure from the
  Ruuvis, Orion detail (starter V, state), MultiPlus detail.
- LEDC backlight dimming on GPIO 38; low-SOC alert.
