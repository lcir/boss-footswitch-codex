# Katana Footswitch

ESP-IDF firmware for a DIY `Boss Katana-50 Gen 3` footswitch that talks to the amp over `BT-DUAL` and exposes the same controls through a local web UI.

## What is implemented

- Application state model for `A1 / A2 / B1 / B2 / SOLO / MODE`
- Single action pipeline shared by hardware buttons and web UI
- BLE transport for `Katana-50 Gen 3` over standard `BLE-MIDI`
- Persistent `Gen 3` MIDI mapping in `NVS` with web-based calibration
- WiFi provisioning flow with SoftAP bootstrap and stored credentials in `NVS`
- HTTP API, WebSocket state broadcast and embedded mobile-first web interface
- LED state renderer and hardware pin map for six RGB indicators

## Current status

The repo now builds as a real `ESP-IDF 5.x` firmware and uses a `Gen 3 MIDI-only` transport:

- `main/amp_transport.c` scans for `KATANA 3 MIDI`, connects, pairs, discovers the standard BLE-MIDI service/characteristic and enables notifications.
- Preset switching uses configurable `Program Change` numbers and effect toggles use configurable `Control Change` numbers.
- The web UI exposes the MIDI mapping, `PC offset` calibration flow and `SOLO` enablement.
- Default mappings follow the `BOSS Tone Studio for KATANA Gen 3` receive screen:
  - `A1=1`, `A2=2`, `B1=6`, `B2=7`, `PANEL=5`
  - `Booster=CC16`, `Mod=CC17`, `FX=CC18`, `Delay=CC19`, `Reverb=CC20`, `Send/Return=CC21`
- `SOLO` intentionally has no hardcoded default and stays disabled until configured in the UI.

Still deferred:

- authoritative amp state feedback back into the controller
- the final `WS2812/SK6812` low-level LED transmit backend in `main/leds.c`

The runtime no longer sends any `Gen 2/MKII`-style state query traffic. That is a hard safety rule after earlier `BT-DUAL` experiments destabilized the amp UI.

## Project layout

- `main/app_main.c`: startup, task wiring and central action queue
- `main/app_state.*`: authoritative runtime state shared by hardware and web
- `main/buttons.*`: GPIO scanning, debounce, long-press and WiFi reset combo
- `main/midi_config.*`: NVS-backed `Gen 3` PC/CC mapping and calibration state
- `main/wifi_manager.*`: NVS-backed credentials and AP/STA switching
- `main/web_server.*`: REST API, MIDI config endpoints, provisioning endpoint and WebSocket broadcast
- `main/web_ui.*`: embedded HTML/CSS/JS for the virtual footswitch
- `main/amp_transport.*`: `Gen 3` BLE-MIDI transport and outbound `PC/CC` writes
- `docs/hardware.md`: wiring and pin assumptions
- `docs/protocol-capture.md`: `Gen 3` MIDI mapping notes and future feedback investigation

## HTTP API

- `GET /api/state`
- `POST /api/action`
- `GET /api/midi-config`
- `POST /api/midi-config`
- `POST /api/midi-calibrate`
- `POST /api/provision`
- `GET /ws`

`GET /api/state` now includes:

- `midiConfigured`
- `soloConfigured`
- `pcOffsetMode`
- `ampStateConfidence`

`POST /api/midi-calibrate` is a two-step helper:

1. send a temporary `A1` test with `candidate=subtract-one` or `candidate=direct`
2. after the amp switches correctly, confirm the chosen candidate and persist it

Until `pcOffsetMode` is calibrated, preset switching stays disabled in the UI.

## Build notes

This is structured as a normal ESP-IDF app:

```sh
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

## ESP-IDF prerequisites

This project was verified against a local ESP-IDF checkout in `~/esp/esp-idf`.

If `idf.py` does not work immediately, initialize the toolchain in this order:

```sh
bash ~/esp/esp-idf/install.sh esp32
source ~/esp/esp-idf/export.sh
idf.py set-target esp32
idf.py build
```

Notes:

- `install.sh esp32` is needed at least once to create the Python environment under `~/.espressif/python_env/...` and install required Python packages such as `click`.
- `source ~/esp/esp-idf/export.sh` must be run in each new shell before calling `idf.py`.
- The project requires `CONFIG_HTTPD_WS_SUPPORT=y` because the web UI uses WebSockets through `esp_http_server`.
- The current build assumes a `4MB` ESP32 module and uses the `single_app_large` partition layout.
- If you need to regenerate `sdkconfig` from defaults, remove `sdkconfig` and rerun `idf.py build`.

Verified output:

- app image: `build/boss_footswitch.bin`
- partition headroom: about `30%` free in the `1500K` factory app partition

## BTS setup checklist

Before testing the controller against the amp, verify these receive mappings in `BOSS Tone Studio for KATANA Gen 3`:

- `RX PC PANEL = 5`
- `RX PC A: CH-1 = 1`
- `RX PC A: CH-2 = 2`
- `RX PC B: CH-1 = 6`
- `RX PC B: CH-2 = 7`
- `RX CC BOOSTER SW = 16`
- `RX CC MOD SW = 17`
- `RX CC FX SW = 18`
- `RX CC DELAY SW = 19`
- `RX CC REVERB SW = 20`
- `RX CC SEND/RETURN SW = 21`

`SOLO` must be configured manually in the web UI if you want the dedicated solo button to send MIDI.

## CLion

The repo includes [CMakePresets.json](/Users/peny/Development/Projects/boss-footswitch-codex/CMakePresets.json) with ready-to-use ESP-IDF profiles for CLion.

Use it like this:

1. Open the project in CLion.
2. When CLion offers CMake presets, select `esp-idf-debug`.
3. Let CLion reload CMake with that preset.
4. Build normally from the hammer icon or the CMake tool window.

What the preset already provides:

- `IDF_PATH=/Users/peny/esp/esp-idf`
- ESP-IDF Python env on `PATH`
- Xtensa toolchain on `PATH`
- `IDF_TARGET=esp32`
- `CMAKE_EXPORT_COMPILE_COMMANDS=ON`

Available presets:

- `esp-idf-debug`
- `esp-idf-release`

If CLion was already open before the preset was added, use `File -> Reload CMake Project` or reopen the project.
