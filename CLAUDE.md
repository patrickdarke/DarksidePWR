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
cp secrets.h.example secrets.h                  # Wi-Fi creds only; config.h has the rest
./build.sh                                      # compile
./build.sh flash                                # compile + flash attached panel
```

`secrets.h` is gitignored — NEVER put real credentials in a tracked file
(this repo's history was verified clean before first push; keep it that
way). Since the config split it holds ONLY Wi-Fi credentials; all GX
addressing/unit-id/display config lives in the committed `config.h`
(#ifndef-guarded, so secrets.h may override any define). The local
secrets.h predates the split and still carries overrides — that's fine,
they win over config.h by design.

Wi-Fi credentials are best set ON DEVICE: gear button (lower right) → Wi-Fi
setup screen (scan/pick/type). Saved to NVS namespace `darkside`, keys
`wifi.ssid`/`wifi.pass`; at boot NVS wins over the secrets.h fallback, and
boot telemetry says which was used: `[pwr] wifi connecting to <ssid> (nvs)`
vs `(secrets.h)`. So a fresh clone builds and provisions with the example's
placeholder credentials untouched. NVS survives reflashes (the app3M_fat9M
partition table keeps the nvs partition); `esptool erase-flash` clears it.

## Hardware (CrowPanel Advance 3.5", board rev V1.2–V1.4)

- ESP32-S3-WROOM-1-N16R8: 16 MB flash, 8 MB **octal** PSRAM (`PSRAM=opi`),
  native USB CDC — enumerates as `/dev/cu.usbmodem*`, no UART bridge.
- Panel: ILI9488, 40 MHz SPI — SCLK 42, MOSI 39, DC 41, CS 40, RST 2;
  `offset_rotation=3` → 480×320 landscape; `invert=true`.
- Touch: GT911 I²C — SDA 15, SCL 16, INT 47, RST 48, addr 0x14 (wired into
  LVGL; used by the gear button → Wi-Fi setup screen).
- Backlight: LEDC PWM on GPIO 38 (20 kHz, 10-bit — backlight.cpp). Percent
  persists in NVS `darkside`/`bright`, floored at `kBacklightMinPct` = 10%
  so a touch-only device can never dim itself to an unreadable screen.
- Expansion headers: J13 = I²C (IO15/16, shared with touch), J15 = UART1
  (IO17/18). Board has TF slot + mic (pins unverified).
- Sound (schematic-verified, V1.4 PDF in the vendor repo):
  - Passive piezo BEEP_5025 on IO8 (net `IO8_BEEP`, SS8050 driver) — THE
    alert path. beeper.cpp drives it with LEDC at ~4 kHz (its resonance);
    chirps play on a short task, never on the LVGL loop.
  - NS4168 I2S mono class-D amp → speaker: BCLK 13, LRC 11, SDATA 12,
    CTRL = IO21 (the vendor "pull 21 low" quirk is this pin; firmware
    still parks it LOW). DEPRECATED for alerts 2026-07-24 — the piezo
    replaced the I2S chime — but the path is verified working (ESP_I2S,
    begin/end per sound; see git history) if music/voice is ever wanted.
  - PDM mic on IO9 CLK / IO10 DATA, shared with the wireless-module SPI
    pads; unused.
  Full-charge chirps: arm after 30 consecutive charging samples, fire once
  when charging stops with SOC >= kChimeSocPct (99, in the .ino), re-arm
  next session. Serial 'B' plays them on demand.
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
- HWCDC backpressure law (cost a fake 6 s "UI stall"): a host that OPENS the
  CDC port but pauses draining makes every Serial print block for its TX
  timeout — the loop froze seconds at a time. Firmware runs
  `Serial.setTxTimeoutMs(0)` (drop, never block); the screenshot dump
  temporarily restores 250 ms so frames arrive intact. If `[ui] max loop
  gap` ever spikes while a capture/monitor script is mid-setup, suspect the
  host, not the firmware.
- Debug serial commands (handled in loop()): `S` = screen dump as hex
  RGB565 (decode: `tools/capture_screenshot.py out.png`, pre-cmd args like
  `K 1.5` open a screen first), `U` = setup screen, `C` = control page,
  `B` = chirps, `V` = vebus unit sweep, `K` = setup + keyboard up, `D` =
  arm the MULTI OFF confirm (real first-tap path, never confirms).
  Screenshots in docs/img were made this way — regenerate on UI changes and
  keep USERGUIDE.md / SETUP.md in sync.
- LVGL KEYBOARD LAW (cost an invisible-keyboard regression): lv_keyboard
  self-aligns BOTTOM_MID at creation, so lv_obj_set_pos() coordinates become
  OFFSETS from that anchor — the keyboard sat 16 px past the screen bottom
  and the screen silently scrolled to chase it (the real cause of the
  "keyboard too low" feedback). Dock keyboards with lv_obj_align(...,
  LV_ALIGN_BOTTOM_MID, 0, 0); setup screen has SCROLLABLE cleared so focus
  changes can never shift the layout again.

## Toolchain (pinned, vendored)

- esp32 core **3.3.10** (same as DSOdash fleet — do not drift casually).
- `lib/` vendors the display stack; `--libraries ./lib` in build.sh:
  - LVGL **8.3.11** — ELECROW's exact copy + their `lv_conf.h` (montserrat
    12/14/20/28/48 enabled; LVGL v8 API: draw_buf/disp_drv, label recolor).
    One local conf change: `LV_USE_SNAPSHOT 1` (serial screenshot dump).
    Montserrat glyph range is ASCII+° only — no middle dot `·` (renders as
    a box; footer uses triple-space separators instead).
  - LovyanGFX **1.2.26** — DELIBERATE upgrade from vendor's 1.1.16, which
    does not compile against core 3.3.10/IDF 5.5 (i2c_periph_signal.module,
    lcd_periph_signals errors). Same config API.
  - LovyanGFX's CJK font dirs (`src/lgfx/Fonts/IPA`, `Fonts/efont`) are
    hard-`#include`d by `lgfx_fonts.cpp` — they CANNOT be trimmed.
- FQBN: `esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB`.
- Dual full-frame LVGL buffers in PSRAM (vendor lesson pattern).

## The Victron system (data source)

Ekrano GX v3.75, `venus.local` / fallback `10.20.30.239` (DHCP reservation),
VRM installation "Darkside.Overland".
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
| 100 | settings | 5700 | system name, string[8], 2 ASCII/reg hi-first — in NO released firmware yet: added to dbus_modbustcp 2026-03-17 (4e566cf3, which also added per-device CustomName regs, e.g. tank 3010, temp 3320); v3.75 (current latest) answers exception 2, verified live 2026-07-24 | text |
| GX_VEBUS_UNIT | vebus (MultiPlus) | 33 W | /Mode: 1 chg-only, 2 inv-only, 3 on, 4 off | 1 |
| GX_VEBUS_UNIT | vebus | 22 W | /Ac/ActiveIn/CurrentLimit (shore limit) | ÷10 A |
| 100 | system | 806/807 W | GX relay 0/1 state (0 open, 1 closed; relay function must be Manual) | 1 |
| 100 | settings | 2705 W | DVCC max charge current, -1 = no limit | A |
| 239 | alternator | 4119 W | Orion /Mode 1 on / 4 off — same firmware vintage as 5700, expect exception 2 on ≤3.75 (probed once per connection) | 1 |

### Reliability laws

- ALL GX I/O (mDNS resolve, connect, every register read) runs on a core-0
  FreeRTOS task ("gxpoll", spawned by `gxStart()`); the LVGL loop on core 1
  never blocks on the network. Rationale, measured 2026-07-24: inline
  polling froze the loop 250–1800 ms per second on a degraded GX link —
  every touch felt laggy. Samples cross via a mutexed snapshot + sequence
  number (`gxSnapshot`); the loop prints telemetry when a new sample lands
  (`poll=NNNms` = poll duration on the task, informational). `[ui] max loop
  gap NNNms` every 10 s is the UI-health metric — single digits is healthy.
  Setters are task-safe: temps unit is an atomic flag; a GX-target change
  writes NVS then posts to the task (never touch its socket from the UI).

- System reads (unit 100) are FATAL on failure → drop socket, 3 s backoff,
  re-resolve on reconnect. Sensor reads (alternator/temps/tanks) are
  **per-device NON-FATAL**: a dead/napping device shows `--` (or 0 W for the
  alternator), never stale numbers, never a failed poll. A month-dead Mopeka
  ("Elijah Craig") drops off D-Bus entirely — its unit stops answering; this
  is normal and self-heals when the sensor returns.
- `mbRead`/`mbWrite` (modbus_transport.cpp) parse the MBAP length before the
  body so Modbus exceptions are consumed cleanly (no socket desync, no
  500 ms stall). They return a three-way result: ok / kRegException (clean
  exception, socket fine) / kRegFault (timeout, framing, or TID mismatch —
  bytes may still be in flight).
- MODULE MAP since the review refactor: modbus_transport = socket+framing
  only; gx_poller = register map, GxData, the core-0 task, write queue,
  sweep; gx_settings = NVS runtime settings (own mutex, pending-target
  handoff); config.h = committed non-secret defaults (#ifndef-guarded so
  secrets.h can override any of them); secrets.h = Wi-Fi credentials only.
- A FAULT during the sensor reads skips the remaining sensors for that poll
  (each would stall toward its 500 ms deadline on a desynced stream) and
  cycles the socket with NO backoff — the GX just answered the system reads,
  so the box is up. The poll still succeeds with the system values; the
  skipped sensors show `--` for that one poll. Telemetry signature:
  `[gx] sensor read fault, cycling connection`.

## UI map (ui.cpp + ui_setup.cpp, palette in ui_theme.h)

SOC arc (teal) + big % + CHARGING/DISCHARGING/IDLE · tiles: HOUSE V /
CURRENT A (amber) / POWER IN W (green, = PV + alternator) / AC LOADS W
(blue) · footer: temps line (°F or °C per `TEMPS_IN_F`, default F), tanks
line (red below `kTankLowPct = 20`%), power line (PV · ALT · DC · NET), gear
button (432,280) → setup screen. Link dot: red = no Wi-Fi, amber = Wi-Fi but
no GX, green = live (10 s staleness window). Sensor unit-ids/labels/°F-°C
are config in `secrets.h`; sensor COUNTS derive from the GX_*_UNITS list
lengths (ui.cpp static_asserts the label lists match; footer fits ~3 per
line). Threshold in `ui.cpp`.

Header title: `UI_TITLE` in secrets.h wins; `""` (or a secrets.h from before
the define existed — #ifndef fallback) = auto-pull the GX system name (VRM
installation name, reg 5700, uppercased; "PWR MONITOR" until it arrives).
Auto-pull cannot work on ANY current GX: reg 5700 is in no released
firmware (v3.75 is the latest and answers exception 2 — the panel logs this
once per connection and falls back cleanly), so the local secrets.h pins
UI_TITLE "DARKSIDE  PWR". The first Venus release cut after 2026-03-17
enables it; names longer than 16 chars will arrive truncated (string[8]).

CONTROL page (ui_control.cpp, bolt chip on the main screen; serial 'C'):
MultiPlus mode segmented buttons (OFF needs a second confirming tap — it
kills AC loads), shore-limit stepper (5 A steps, 5-50), GX relay toggles,
DVCC charge-limit stepper (10 A steps, 10-100 then NO LIMIT), alternator
ON/OFF (hidden as "needs newer GX firmware" when reg 4119 answers
exception). WRITE LAW: writes queue via gxWrite() to the poller task (FC6,
same framing/tri-state as reads; never touch the socket from the UI), the
task logs "[ctl] write ..." and immediately re-polls, and every widget
highlights GX READ-BACK state — a rejected or overridden write (DMC/BMS can
own the shore limit; ext-control solar ignores /Mode) shows itself within a
second. Telemetry gained mp/sh/r1/r2/chg/am fields. Serial 'V' sweeps units
200-247 for vebus /Mode to find GX_VEBUS_UNIT on a new install.

Setup screen (ui_setup.cpp): async Wi-Fi scan (strongest-first, deduped, top
12) into a tappable list; SSID + password + GX-target textareas (password
mode on; GX field takes an mDNS host or IP, cleared = secrets.h default,
custom hosts do NOT fall back to the pinned IP) with the LVGL keyboard
(480x152 at y168 — 36 px input rows keep all three fields visible above it,
keys stay finger-sized); UNITS °F/°C
toggle button (applies immediately, like brightness — CANCEL doesn't undo
either); brightness slider; status line lives in the header. SAVE applies
only fields that differ from their open-time prefills — notably Wi-Fi is
untouched unless the SSID changed or a password was typed, so saving other
settings can NEVER wipe a stored password. NVS keys (namespace `darkside`):
wifi.ssid, wifi.pass, bright, gx.addr, tempF — secrets.h supplies every
first-boot default.
SCAN LAW (cost a live debug): esp_wifi_scan_start FAILS while a join attempt
is in flight, and a bad SSID keeps the radio in a join-retry loop — so
startScan aborts a non-connected join first, boot never joins placeholder
creds, and CANCEL-without-save restarts the stored-credential join.
Tapping a network jumps straight to password entry. SAVE persists to NVS and
re-joins immediately (main screen's dot shows progress); CANCEL (top right,
kRing chip — kTile chips are invisible on the physical panel) discards.
Telemetry: `[setup] scan started` / `scan done: N seen, M listed` /
`saved ssid=..., joining` (the password is never logged).

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
- Low-SOC alert. (LEDC dimming is DONE — setup-screen slider; auto day/night
  dimming could build on it.)
