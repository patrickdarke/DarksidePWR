# Darkside PWR — project laws and hard-won facts

Victron power monitor, now TWO installs in one repo, selected at compile
time (`BOARD_CROWPANEL_50`, see "Board switch" below):
- The truck ("Darkside.Overland" system) on the ELECROW CrowPanel Advance
  3.5" — the original install, default build target.
- A second, separate new install (own Victron GX, own device roster — two
  MPPT chargers, Orion DC-DC, one LPG tank, a SmartShunt + two Bluetooth
  smart batteries, no Ruuvi temps) on the ELECROW CrowPanel Advance 5.0"
  (800x480), added 2026-07-31. Not a bigger-screen reskin of the truck's
  panel — different display bus (RGB-parallel vs SPI), different backlight
  mechanism, several GPIOs mean different things. See its own Hardware
  section below; do not assume 3.5" facts carry over.

Sibling of the DSODash project and follows its design language (dark
`0x0C1018` bg, tiles `0x111826`, teal/green/amber/blue accents, montserrat)
and its working rules. Read this file before touching anything — every fact
here was verified on hardware or against a live GX, and several cost real
debugging time. Facts explicitly marked TODO/unverified are exactly that —
don't treat them as confirmed.

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
- Case: `case/darksidepwr-case.3mf` (print-ready Bambu Studio project) +
  `.step` (CAD source, Shapr3D export) — owner-designed enclosure for the
  3.5" panel, added 2026-07-25. Photo of the printed case:
  `docs/img/case-photo.jpg` (hero image in README + web flasher; EXIF/GPS
  stripped — keep it that way if ever replaced).
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
- HWCDC backpressure law (cost a fake 6 s "UI stall"; the API used here,
  `setTxTimeoutMs`, is HWCDC/native-USB-CDC-only and doesn't exist on
  `HardwareSerial` — `#if !defined(BOARD_CROWPANEL_50)`-guarded in
  `DarksidePWR.ino`; the 5" board needs its OWN fix for the same underlying
  problem, see the next bullet — it is NOT immune): a host that OPENS the
  CDC port but pauses draining makes every Serial print block for its TX
  timeout — the loop froze seconds at a time. Firmware runs
  `Serial.setTxTimeoutMs(0)` (drop, never block); the screenshot dump
  temporarily restores 250 ms so frames arrive intact. If `[ui] max loop
  gap` ever spikes on the 3.5" board while a capture/monitor script is
  mid-setup, suspect the host, not the firmware.
- UART backpressure law, 5" board (found during first bring-up, 2026-07-31
  — same underlying problem as the HWCDC law above, worse because
  `HardwareSerial` has no equivalent fix available): a host that has the
  port open but stops draining it (e.g. plugged into a laptop for power
  with no program reading the port) blocks every `Serial` write until
  there's room — this is NOT limited to native-USB-CDC. **Measured live**:
  a screenshot dump interrupted mid-capture left the board's UART TX
  blocked, and `loop()` froze for **406515 ms** once the recurring
  telemetry hit the same wall. Fix: `DarksidePWR.ino`'s `telPrintln()`
  checks `Serial.availableForWrite()` and drops the line instead of
  blocking, for the two lines that run forever in normal operation (the
  ~1 Hz `[gx] ...` telemetry, the 10 s `[ui] max loop gap` line) —
  `BOARD_CROWPANEL_50`-only, a no-op on the 3.5" board (whose HWCDC fix
  above already covers it). Explicit debug output (`dumpScreen()`, the
  `'M'`/`'V'` command replies) is deliberately exempt — same tradeoff the
  3.5" board's screenshot dump already makes: losing bytes there is worse
  than the stall.
- WEB FLASHER: https://patrickdarke.github.io/DarksidePWR/ (GitHub Pages
  from /docs; esp-web-tools + docs/manifest.json + the MERGED image
  docs/firmware/darksidepwr.bin, gitignore-excepted). Rebuild the image
  with tools/build_webflash.sh (builds NEUTRAL from secrets.h.example —
  moves any local secrets.h aside untouched) and commit the new bin
  whenever main changes user-visibly; the on-device setup screen is what
  makes a stock image usable.
- Debug serial commands (handled in loop()): `S` = screen dump as hex
  RGB565 (decode: `tools/capture_screenshot.py out.png`, pre-cmd args like
  `K 1.5` open a screen first), `U` = setup screen, `C` = control page,
  `B` = chirps, `V` = unit sweep (solar + vebus), `K` = setup + keyboard up, `D` =
  arm the MULTI OFF confirm (real first-tap path, never confirms), `M` = heap/PSRAM
  watermarks (LV_MEM_CUSTOM=1: heap-free IS the LVGL watermark).
  Screenshots in docs/img were made this way — regenerate on UI changes and
  keep USERGUIDE.md / SETUP.md in sync.
- LAYOUT POLICY (owner decision 2026-07-24): screens use absolute pixel
  coordinates and STAY that way — do not convert to flex/grid or introduce
  layout constants; the screens are hardware-verified as-is. **Revisited
  2026-07-31**: a second panel size did become a target (the 5.0" board
  below) — the policy itself didn't change, each board just gets its own
  set of absolute-pixel constants (see "Board switch"), never a shared
  dynamic layout.
- LVGL KEYBOARD LAW (cost an invisible-keyboard regression): lv_keyboard
  self-aligns BOTTOM_MID at creation, so lv_obj_set_pos() coordinates become
  OFFSETS from that anchor — the keyboard sat 16 px past the screen bottom
  and the screen silently scrolled to chase it (the real cause of the
  "keyboard too low" feedback). Dock keyboards with lv_obj_align(...,
  LV_ALIGN_BOTTOM_MID, 0, 0); setup screen has SCROLLABLE cleared so focus
  changes can never shift the layout again.
- SELF-REFERENCE LAW (cost a live "CANCEL does nothing" debug session,
  found 2026-08-01): `uiSetupOpen()`/`uiCtlOpen()` capture "the screen to
  return to" via `s_prevScr = lv_scr_act()`. Called while THAT screen is
  already active (e.g. the serial `'U'`/`'C'` debug commands sent twice in
  a row without navigating away in between) it captures itself, and
  CANCEL/SAVE/CLOSE's `lv_scr_load(s_prevScr)` becomes a no-op — indistin-
  guishable from the button doing nothing at all. Both now guard with
  `if (lv_scr_act() != s_scr) s_prevScr = lv_scr_act();`. If a button ever
  again "does nothing," check this before suspecting touch/hardware.

## Hardware (ELECROW CrowPanel Advance 5.0", board rev V1.2, SKU DISO2050A)
— second install, added 2026-07-31

800x480 IPS panel, own Victron GX (not yet commissioned — see "Board
switch" for where its config lives). Same ESP32-S3-WROOM-1-N16R8 chip as
the 3.5" board (16 MB flash, 8 MB octal PSRAM) — but almost everything else
about the hardware layer differs. All facts below verified against
ELECROW's vendor repo (`Elecrow-RD/CrowPanel-Advance-5-HMI-ESP32-S3-AI-Powered-IPS-Touch-Screen-800x480`,
`example/V1.2_and_V1.3/Arduino/lesson-03/BigInch_LVGL/`) and the owner's
photo of the physical board silkscreen (SKU + "V1.2" printed on the PCB),
not guessed.

- **Display**: ST7262 RGB-parallel TFT (not SPI) — 20 GPIOs: 16 data lines
  (D0-D15) + HSYNC(40)/VSYNC(41)/PCLK(39)/H-Enable(42). Landscape-native, no
  `offset_rotation` trick needed (unlike the 3.5"'s ILI9488). LovyanGFX
  `Bus_RGB`/`Panel_RGB` (already present in the vendored 1.2.26 copy, no
  library changes needed). Flush uses `gfx.pushImageDMA()` between a
  conditional `endWrite()`/no `setAddrWindow`, not the SPI panel's
  `setAddrWindow`+`writePixels` — see `DarksidePWR.ino`'s `dispFlush()`.
- **Touch**: GT911, same physical I2C pins as the 3.5" (SDA 15 / SCL 16,
  shared bus) but address **0x5D** (not 0x14). No INT/RST wired
  (`pin_int=-1`/`pin_rst=-1`) — already poll-based via `gfx.getTouch()`,
  same as the 3.5" build.
- **Backlight — no LEDC PWM pin available** (GPIO 38, the 3.5" board's
  backlight pin, is RGB data line B4 here). Controlled by an onboard
  supervisor MCU at **I2C address 0x30**: single-byte command, 0 =
  brightest, 245 = off (linear, inverted vs. a percent — see
  `backlight.cpp`'s `BOARD_CROWPANEL_50` branch). Bring-up needs a
  detect/handshake loop over `Wire` (same 15/16 bus as touch) that MUST run
  before LovyanGFX's own touch I2C init claims the port — this is why
  `backlightInit()` runs before `gfx.init()` in `DarksidePWR.ino`, for both
  boards now (harmless reorder on the 3.5").
- **USB is a WCH UART bridge, not native CDC — confirmed live** (plugged
  into a Mac and inspected via `system_profiler`/`ioreg`): vendor
  `0x1a86` (QinHeng/WCH), product `0x7522`, USB class `0xFF`
  (vendor-specific) — not Espressif's `0x303a` native-CDC ID the 3.5" board
  reports. Matches the silkscreen (`IO43-TX0`/`IO44-RX0` routed to the USB
  connector). **Needs a driver**: macOS has no built-in driver for a
  vendor-specific USB class — install + approve WCH's `CH34xVCPDriver`
  (System Settings → General → Login Items & Extensions → Driver
  Extensions) before anything can see the port. Confirmed enumeration once
  approved: `/dev/cu.wchusbserial*` (NOT `cu.usbmodem*`) — `build.sh`'s
  port glob for board `50` matches this.
- **Beeper**: a dedicated BUZZER footprint is confirmed present on the
  physical board (silkscreened next to the I2C-OUT header, its own driver
  transistor) — but its GPIO is not silkscreened and wasn't in the fetched
  vendor example code. **TODO, unverified**: `beeper.cpp`'s
  `BOARD_CROWPANEL_50` branch ships with `BEEPER_PIN_50 = -1` (chime
  disabled, safe no-op) rather than guessing a pin that could collide with
  an already-claimed RGB/I2C/mic line. Override `BEEPER_PIN_50` once the
  real pin is confirmed.
- **Present but unused**: PDM-style mic (IO19 CLK / IO20 DATA — explains
  the vendor bring-up's otherwise-mysterious `pinMode(19, OUTPUT)`, held
  low at boot same idea as the 3.5"'s amp CTRL pin), I2S speaker amp
  (IO4/IO5/IO6), a battery-backed RTC at I2C address 0x51 (CR1220 cell, same
  bus as touch/backlight — genuinely new capability vs. the 3.5" board, not
  wired up). A "Function Select" DIP switch (S0/S1) multiplexes IO19/IO20
  between mic+TF-card mode and other modes (e.g. the optional pluggable
  wireless module) — leave at factory default.

## Board switch (compile-time)

One repo, one macro: `BOARD_CROWPANEL_50`, set via `build.sh`'s `50`
argument (`--build-property build.extra_flags=-DBOARD_CROWPANEL_50`).
Undefined = the 3.5" truck build, unchanged. Two splitting patterns:

- **Vendored/config headers** (`LovyanGFX_Driver.h`, `config.h`): tiny
  `#if defined(BOARD_CROWPANEL_50) #include ..._50.h #else ..._35.h #endif`
  selectors. `LovyanGFX_Driver_35.h`/`_50.h` are ELECROW's lesson-03
  configs, unmodified, for their respective boards. `config_35.h` is the
  truck's committed install config (unchanged content, just renamed).
  `config_50.h` is the second install's device roster — Victron unit IDs
  are placeholders (0) until that GX is commissioned; discover them with
  the serial `'V'` sweep per SETUP.md and fill in (or override from
  secrets.h). `GX_SOLAR_UNITS`/`GX_SOLAR_LABELS` is a new **plural** define
  (both boards now use it — `config_35.h`'s single MPPT is `{1}`), mirroring
  the existing `GX_TEMP_UNITS`/`GX_TANK_UNITS` list pattern; `gx_poller`'s
  `GxData::solarMode[]`/`solarModeOk[]` are arrays sized `kNumSolar` for the
  same reason (the second install has two MPPT chargers, each independently
  controllable on the CONTROL page).
- **Implementation files** (`backlight.cpp`, `beeper.cpp`, `ui.cpp`,
  `ui_control.cpp`): same filename, whole-file
  `#if defined(BOARD_CROWPANEL_50) ... #else ... #endif` — two complete,
  independent implementations behind the same header interface. Chosen over
  `_35`/`_50`-suffixed files for these because it keeps a single build unit
  per concern and git diffs obviously scoped. `ui_setup.cpp` is the one
  exception: its Wi-Fi scan/save/cancel state machine is entirely
  board-agnostic (no screen-coordinate dependence), so only `uiSetupBuild()`
  itself is guarded — duplicating ~300 lines of identical, behavior-
  sensitive logic (e.g. the password-preservation rule in `saveCb`) for two
  boards would just be a drift risk with no benefit.
- **`DarksidePWR.ino`**: can't be split (one sketch entry point) — small
  `#if` regions for `kLcdW`/`kLcdH`, `dispFlush()`, and the gfx
  init sequence (`gfx.init()+initDMA()+startWrite()` for RGB vs `gfx.begin()`
  for SPI).
- **UI redesign, not a rescale**: the 5.0" screens (`ui.cpp`,
  `ui_control.cpp`, `ui_setup.cpp`) are genuinely different layouts for the
  800x480 canvas — bigger SOC arc, a 3x2 tile grid that shows PV and ALT as
  separate tiles (previously combined into one "POWER IN" number on the
  smaller screen; both are still the same GX system-aggregate reads, just
  no longer summed), one labeled ON/OFF row per solar charger on CONTROL.
  Still no custom font — reuses the same montserrat 12/14/20/28/48 sizes
  already enabled in `lv_conf.h`.
- **Known open TODOs** (grep `BOARD_CROWPANEL_50`/`TODO` for exact spots):
  buzzer GPIO, per-charger solarcharger PV-power register (`solarPvW` in
  `gx_poller.h` — unit 100 reg 850 is the only solar-power register this
  project has confirmed; per-charger needs checking against
  `victronenergy/dbus_modbustcp`'s attributes.csv or live experimentation),
  the `50` FQBN's `CDCOnBoot` value (untested against a real flash), and
  every real Victron unit ID in `config_50.h`.

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
    lcd_periph_signals errors). Same config API. Already vendors
    `Bus_RGB`/`Panel_RGB` for esp32s3, used by the 5.0" board.
  - LovyanGFX's CJK font dirs (`src/lgfx/Fonts/IPA`, `Fonts/efont`) are
    hard-`#include`d by `lgfx_fonts.cpp` — they CANNOT be trimmed.
- FQBN: `esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB`
  for the 3.5" board (`CDCOnBoot=cdc` = native USB). The 5.0" board's FQBN
  in `build.sh` uses `CDCOnBoot=default` instead — it has no native-CDC USB
  wiring (see its Hardware section) — untested against a real flash yet.
- Dual full-frame LVGL buffers in PSRAM (vendor lesson pattern). On the
  5.0" board this is ~1.5 MB (800x480x2Bx2 buffers) plus LovyanGFX's own
  internal RGB frame buffer (~0.75 MB) — comfortably inside the 8 MB octal
  PSRAM budget, not a concern.

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
| 239 | alternator | 4119 W | Orion /Mode 1 on / 4 off — WORKS on v3.75 (verified live 2026-07-24, unlike 5700); still probed once per connection for older firmware | 1 |
| 1 | solarcharger (VE.Can MPPT) | 774 W | /Mode 1 on / 4 off. VE.Can instances are >247 and reachable ONLY via the static unitid2di map — unit 1 on this system, found by the 'V' sweep; never assume unit = VRM instance for VE.Can devices. VERIFIED live 2026-07-24: /Mode writes STICK even with the MPPT under DVCC/ext control (off->on round trip by owner) | 1 |

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

## UI map (ui.cpp / ui_setup.cpp / ui_control.cpp; palette ui_theme.h,
shared button factory ui_widgets.h)

SOC arc (teal) + big % + CHARGING/DISCHARGING/IDLE · tiles: HOUSE V /
CURRENT A (amber) / POWER IN W (green, = PV + alternator) / AC LOADS W
(blue) · footer: temps line (°F or °C per `TEMPS_IN_F`, default F), tanks
line (red below `kTankLowPct = 20`%), power line (PV · ALT · DC · NET), gear
button (432,280) → setup screen. Link dot: red = no Wi-Fi, amber = Wi-Fi but
no GX, green = live (10 s staleness window). Sensor unit-ids/labels/°F-°C
are config in `config.h` (secrets.h may override); sensor COUNTS derive
from the GX_*_UNITS list lengths — `{}` is valid, the lines just vanish
(GxData arrays pad to >=1 so empty stays legal C++). ui.cpp static_asserts
the label lists match; footer fits ~3 per line. Threshold in `ui.cpp`.

Header title: `UI_TITLE` in config.h wins (secrets.h may override it);
`""` = auto-pull the GX system name (VRM
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
ON/OFF (noted "needs newer GX firmware" when reg 4119 answers exception),
solar ON/OFF (SmartSolar /Mode reg 774, one row per unit in `GX_SOLAR_UNITS`
— a list now, not a single unit, see "Board switch"; ext-control may
override — read-back shows truth). Six rows, 44 px pitch, 36 px chips. WRITE LAW: writes queue via gxWrite() to the poller task (FC6,
same framing/tri-state as reads; never touch the socket from the UI), the
task logs "[ctl] write ..." and immediately re-polls (queued commands
EXPIRE after 10 s — a tap during an outage must not fire on reconnect),
and every widget
highlights GX READ-BACK state — a rejected or overridden write (DMC/BMS can
own the shore limit; ext-control solar ignores /Mode) shows itself within a
second. Telemetry: mp/sh/r1/r2/chg/sm/am fields (sm is slash-joined per
solar unit, e.g. `sm=1/4`). Serial 'V' sweeps units 1-247 for solarcharger
/Mode (774) and 200-247 for vebus /Mode (33) to find the `GX_SOLAR_UNITS`
entries / `GX_VEBUS_UNIT` on a new install — it already reports every
matching unit, so multiple solar chargers show up in one sweep.

Setup screen (ui_setup.cpp): async Wi-Fi scan (strongest-first, deduped, top
12) into a tappable list; SSID + password + GX-target textareas (password
mode on; GX field takes an mDNS host or IP, cleared = config.h default,
custom hosts do NOT fall back to the pinned IP) with the LVGL keyboard
(480x152 at y168 — 36 px input rows keep all three fields visible above it,
keys stay finger-sized); UNITS °F/°C
toggle button (applies immediately, like brightness — CANCEL doesn't undo
either); brightness slider; status line lives in the header. SAVE applies
only fields that differ from their open-time prefills — notably Wi-Fi is
untouched unless the SSID changed or a password was typed, so saving other
settings can NEVER wipe a stored password. NVS keys (namespace `darkside`):
wifi.ssid, wifi.pass, bright, gx.addr, tempF — config.h supplies every
first-boot default (secrets.h only Wi-Fi + optional overrides).
SCAN LAW (cost a live debug): esp_wifi_scan_start FAILS while a join attempt
is in flight, and a bad SSID keeps the radio in a join-retry loop — so
startScan aborts a non-connected join first, boot never joins placeholder
creds, and CANCEL-without-save restarts the stored-credential join.
Tapping a network jumps straight to password entry. SAVE persists to NVS and
re-joins immediately (main screen's dot shows progress); CANCEL (top right,
kRing chip — kTile chips are invisible on the physical panel) discards.
Telemetry: `[setup] scan started` / `scan done: N seen, M listed` /
`saved ssid=..., joining` (the password is never logged).

NIGHT MODE (`night_mode.h`/`.cpp`, **5" board only**, added 2026-08-01):
that install is wall-mounted in a sleeping area, so the display blanks on a
schedule (NIGHT toggle + SLEEP/WAKE steppers, 30-min steps, on the setup
screen below the network list — same immediate-apply-and-persist rule as
UNITS/BRIGHT, no SAVE needed). NTP-timed (`configTzTime("MST7", ...)`,
Arizona Mountain — no DST), **not** the onboard RTC (I2C 0x51): that chip's
register map was never identified from vendor material, so implementing a
driver for it would be guessing at unverified hardware, which this project
doesn't do. Fails safe — the schedule is inert (`enabled=false` default)
until turned on, and does nothing even when on until `time(nullptr)` clears
a ~2023 sanity floor (i.e. the clock has actually synced at least once).
Blanking is real backlight-off (`backlightBlank()`/`backlightWake()` in
backlight.h — bypasses the normal `kBacklightMinPct` floor on purpose,
unlike every other caller of the backlight API), not an LVGL overlay or a
change to the display bus, so it can't reintroduce the tearing issue fixed
earlier. A touch while blanked wakes it for 30s (any touch resets the
countdown) but is **not** forwarded to LVGL as a click — `DarksidePWR.ino`'s
`touchRead()` checks `nightModeTick()` first and swallows that one touch,
so waking the screen in the dark can't also fire whatever button happens to
be underneath. Re-blanks after 30s of no touches, but only while still
inside the sleep window — if the scheduled wake time arrives first, it just
stays on. NVS keys (namespace `darkside`): `night.en`, `night.sm`,
`night.wm` (sleep/wake as minutes-since-midnight, not separate H/M keys).
On the 3.5" board `night_mode.cpp` is a no-op stub (`nightModeTick()`
always returns false) — zero behavior change there, and no `#ifdef` needed
at any call site.

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
