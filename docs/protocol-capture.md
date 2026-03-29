# Katana Gen 3 MIDI Notes

This firmware now treats `Katana-50 Gen 3` as a `Gen 3`-only target and uses standard MIDI over `BT-DUAL`:

- `Program Change` for preset / channel selection
- `Control Change` for effect switches

The transport still uses the standard BLE-MIDI UUIDs:

- service: `03B80E5A-EDE8-4B33-A751-6CE34EC4C700`
- I/O characteristic: `7772E5DB-3868-4112-A1A9-F2669D106BF3`

## Verified Gen 3 receive mapping

These defaults come from the `BOSS Tone Studio for KATANA Gen 3` receive mapping screen and are now the source of truth for the controller defaults:

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

`SOLO` does not have a hardcoded default in firmware and must be configured from the web UI before it will send any MIDI.

## Current runtime behavior

- Scans for a peripheral advertising as `KATANA 3 MIDI`
- Connects as a NimBLE central client
- Pairs, encrypts the BLE link and enables BLE-MIDI notifications
- Sends outbound `Program Change` and `Control Change` messages for footswitch actions
- Keeps inbound notification parsing only for diagnostics
- Does not claim authoritative state sync with the amp

The controller therefore behaves as a safe `write-capable` footswitch, while feedback remains a separate future task.

## PC offset calibration

The BTS UI shows human-facing program numbers, but the BLE wire format may still be zero-based. Firmware therefore stores a `pcOffsetMode`:

- `subtract-one`: BTS visible `1` is sent on wire as `0`
- `direct`: BTS visible `1` is sent on wire as `1`
- `unknown`: preset switching stays disabled

The web UI includes an `A1` test flow to determine which mode works on the real amp, then persist that choice in `NVS`.

## Disabled legacy experiments

Older experiments tried community-documented `Gen 2/MKII`-style query traffic over `BT-DUAL`. That traffic is now explicitly out of scope for runtime firmware.

Operational rule:

- no active `SysEx` state query traffic in default runtime
- no `GATT read` polling against the BLE-MIDI characteristic
- no `Gen 2/MKII` address maps in live transport logic

The reason is simple: those experiments did not produce reliable `Gen 3` feedback and did destabilize the amp UI during testing.

## Future feedback investigation

If authoritative amp feedback is needed later, treat it as a separate `Gen 3` research task:

1. capture real `BOSS Tone Studio for KATANA Gen 3` traffic
2. verify that any readback mechanism is genuinely `Gen 3`
3. only then add a dedicated feedback path without mixing it into the default control runtime
