# Claude Port Refactor Plan

## Goal

Port a small set of high-value ideas from `../boss-footswitch-claude` into this project without weakening the current `Gen 3 MIDI-only` safety model.

Target improvements:

1. Add web-visible runtime diagnostics similar to Claude's BLE log panel.
2. Add `mDNS` hostname support so the device is reachable via `.local`.
3. Improve LED UX for BLE states such as scanning, reconnecting and connected.

## Safety Rules

These rules must remain unchanged during the refactor:

- no outbound `SysEx`
- no inbound `SysEx` parsing
- no `GATT read` polling
- no `Gen 2/MKII` protocol assumptions
- no hardcoded `SOLO` default
- keep `PC offset` calibration and `midiConfigured` gating

## Scope

### In Scope

- BLE / WiFi / action logs exposed in the web UI
- `mDNS` hostname advertisement for HTTP access
- richer LED state rendering for:
  - BLE scanning
  - BLE disconnected / reconnecting
  - successful BLE connect
- small web UI changes needed to display diagnostics

### Out of Scope

- switching to PlatformIO
- switching to Arduino / NimBLE-Arduino
- moving the web UI to LittleFS static assets
- changing the action pipeline to “simulate button press” style
- relaxing the current MIDI config / calibration model

## Recommended Order

### Phase 1: Diagnostics Ring Buffer

Add a small central runtime log buffer and publish log events to the web UI.

Implementation direction:

- introduce a lightweight diagnostics module, for example `main/diag_log.*`
- keep a fixed-size ring buffer of short log entries:
  - timestamp
  - level
  - source
  - message
- log key transitions from:
  - `amp_transport`
  - `wifi_manager`
  - `web_server`
  - top-level action processing in `app_main`
- expose:
  - recent history on initial page load
  - incremental updates over WebSocket

Acceptance criteria:

- no serial monitor needed for basic field debugging
- BLE connect failures, reconnects and config problems are visible in browser
- diagnostics are additive only and do not alter control behavior

### Phase 2: mDNS

Add `.local` access for the web UI in STA mode.

Implementation direction:

- advertise `APP_HOSTNAME` with `ESPmDNS`
- start mDNS only after successful STA connection
- publish HTTP service on port `80`
- surface hostname in `/api/state`

Acceptance criteria:

- device is reachable as `http://katana-foot.local` on supported networks
- feature is inactive in AP provisioning mode
- no regression in WiFi provisioning

### Phase 3: BLE LED Animations

Upgrade LED behavior from static state-only rendering to state-aware animation.

Implementation direction:

- keep the current snapshot-driven render model
- add non-blocking animation state in `main/leds.*`
- map BLE states to patterns:
  - `APP_BLE_SCANNING`: blue sweep
  - `APP_BLE_CONNECTING`: amber pulse
  - `APP_BLE_CONNECTED`: short green confirmation flash, then normal state display
  - `APP_BLE_DISCONNECTED` with reconnect requested: slow warning pulse
- preserve current preset/effect/solo color semantics once connected

Acceptance criteria:

- animations are non-blocking
- no `delay()`-based visual effects
- button handling and web updates stay responsive during animation

## File-Level Notes

Likely write targets:

- `main/app_main.c`
- `main/app_state.h`
- `main/app_state.c`
- `main/leds.h`
- `main/leds.c`
- `main/wifi_manager.c`
- `main/web_server.h`
- `main/web_server.c`
- `main/web_ui.c`
- new module: `main/diag_log.h`
- new module: `main/diag_log.c`

## Design Constraints

- keep ESP-IDF native stack
- keep current REST + WebSocket architecture
- keep the single action pipeline
- do not couple web requests directly to GPIO/button simulation
- prefer concise structured diagnostics over verbose raw dumps

## Nice-to-Have Follow-Up

If the three phases above land cleanly, then evaluate a later fourth phase:

- split the embedded web UI into separate maintainable assets

This is intentionally deferred because it changes packaging and deployment flow without improving BLE/MIDI safety.

## Refactor Prompt Seed

When starting a fresh context, use this summary:

"Port only the safe UX improvements from `../boss-footswitch-claude` into `boss-footswitch-codex`: add a web diagnostics log ring buffer, add mDNS `.local` support, and add non-blocking BLE LED animations. Do not change the `Gen 3 MIDI-only` transport safety rules, do not add `SysEx`, and do not remove `PC offset` calibration or `SOLO` gating."
