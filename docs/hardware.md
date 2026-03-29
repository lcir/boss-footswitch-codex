# Hardware Notes

## Controller

- MCU: `ESP32-WROOM-32`
- Power input: `USB-C 5V`
- Runtime radios: `BLE + WiFi STA`

## GPIO map

- `GPIO32`: switch `A1 / Booster`
- `GPIO33`: switch `A2 / Mod`
- `GPIO25`: switch `B1 / FX`
- `GPIO26`: switch `B2 / Delay`
- `GPIO27`: switch `SOLO`
- `GPIO14`: switch `MODE`
- `GPIO18`: RGB LED data output
- `GPIO19`: level shifter enable

## Switch assumptions

- All six footswitches are momentary, normally-open contacts to ground.
- Inputs use ESP32 internal pull-ups.
- `MODE` short press enters effect mode.
- `MODE` long press returns to preset mode.
- `MODE + SOLO` hold for `2.5s` resets WiFi and re-enters provisioning mode.

## LED assumptions

- Six separate RGB indicators are mounted above the switches.
- The current firmware models them as one daisy-chained addressable strip.
- The intended electrical path is `ESP32 GPIO -> level shifter -> LED DIN`.

## Power path

- `USB-C 5V` input
- Polyfuse on `VBUS`
- ESD protection on connector
- 5V rail for LEDs
- Dedicated `3.3V` regulator for ESP32
- Bulk capacitor near LED chain and local decoupling near ESP32

## Status colors

- Preset active: cyan
- Preset inactive: dim blue
- Effect active: amber
- Effect inactive: dim amber
- Solo active: red
- Mode/runtime: white in preset mode, blue in effect mode, amber while link is reconnecting, red on error
