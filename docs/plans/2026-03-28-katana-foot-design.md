# Katana Footswitch Design

## Summary

- `ESP32-WROOM-32` acts as the single controller for BLE MIDI, WiFi and the local web UI.
- Hardware control layout is fixed to `A1 / A2 / B1 / B2 / SOLO / MODE`.
- Web UI mirrors hardware actions and adds `Reverb`, `BLE reconnect`, `Resync` and provisioning.
- Runtime state is authoritative inside the firmware and broadcast to both LEDs and WebSocket clients.

## Runtime model

- Preset mode: the first four switches select `A1`, `A2`, `B1`, `B2`.
- Effect mode: the first four switches toggle `Booster`, `Mod`, `FX`, `Delay`.
- `SOLO` is always available.
- `MODE` enters effect mode on short press and returns to preset mode on long press.
- `MODE + SOLO` resets WiFi and restarts provisioning.

## Services

- `wifi_manager`: first-boot SoftAP provisioning, then STA reconnect with stored credentials
- `web_server`: `GET /api/state`, `POST /api/action`, `POST /api/provision`, `GET /ws`
- `amp_transport`: BLE-MIDI discovery and dispatch seam for Katana-specific commands
- `buttons`: GPIO scanning and gesture detection
- `leds`: state-to-color renderer for six RGB indicators

## Open implementation seam

- The BLE transport currently keeps a verified integration seam rather than a fully confirmed Katana command table.
- The LED renderer currently computes frames and logs them; the final hardware driver can replace only the emit function.
