# Home Assistant controls

Add the device through Home Assistant's **ESPHome integration**, using its address and the API encryption key from your local secrets. No MQTT broker is needed.

## Firmware profiles

The entity selection is explicit YAML configuration, **not automatic model/capability detection**. Both profiles target the migrated WL6U / RTL8720CF module; other Wi-Fi module hardware is not implied to be compatible.

- `firmware/tac-12chsd.yaml`: the tested TAC-12CHSD selection, with optional entities in `firmware/tac-12chsd-extras.yaml`. Unsupported/extended definitions remain commented out.
- `firmware/all-fields.yaml`: all currently implemented entities, including the uncommented other-model/extended definitions in `firmware/all-fields-extras.yaml`. Optional/raw/extended entities are exposed but disabled by default, including the provisional outdoor-temperature conversion. Everyday controls from `firmware/ac-common.yaml` remain enabled. Unknown protocol bytes and factory/provisioning actions do not become entities.

Choose one top-level profile to compile/upload; `ac-common.yaml` and the extras files are packages, not standalone targets. The former `ac-controls.yaml` entry point is renamed to `tac-12chsd.yaml`; the former `ac-extras.yaml` is now `tac-12chsd-extras.yaml`. Node name, API key and OTA password are unchanged by selecting a profile.

For another AC model, start with `all-fields.yaml`, inspect the definitions, and enable only the entities you intend to test in HA. **Disabled by default does not mean unsupported**, and a visible or echoed-back setting does not prove a working feature. Test one control at a time, compare returned status and actual physical behavior, and record your AC model, reply length and observations. Extended fields may remain unavailable on short status replies; do not bypass freshness/length gates to force them. Some controls have side effects (for example, the eight-degree heat flag changed the setpoint on the tested unit).

HA preserves existing entity enable/disable choices across firmware updates: `disabled_by_default` applies on first discovery, not retroactively. Switching away from a profile that exposed an entity can leave an unavailable registry entry for manual cleanup.

## Entities

| Entity | Behavior |
| --- | --- |
| Air conditioner | Off, auto, cool, dry, fan or heat; target 16–31 °C in 0.5 °C steps |
| Fan | Auto, Quiet, Speed 1–5 |
| Swing | Off, vertical, horizontal or both |
| Presets | None, Eco, Boost or Sleep; selecting one clears the other two |
| Vertical / Horizontal louver | Default, fixed positions and swing ranges |
| Display, Health, Anti-mould, 3D airflow, Light sense | Switches based on the received status |
| Beep | Last value requested by the module, not a reported AC state; defaults to on at boot |
| Sleep level | Numeric control; supported range is defined in the YAML |
| Generator mode | Optional power-limit control (0–3), disabled by default; actual limiting behavior is unverified |
| Room temperature | Received temperature in tenths of a degree Celsius |
| Outdoor temperature (estimated) | Provisional `status[30] - 20` °C conversion; whole-degree resolution |
| Power / Energy | Power derived from energy-counter changes; cumulative energy in kWh |
| Supply voltage / Compressor frequency | **Inferred labels**, not verified measurements; voltage byte can wrap above 255 |
| AC link | Status received within the last ten seconds |
| Error codes | Received numeric codes or `none`; not a diagnosis |

Room-temperature decoding uses the original firmware's packed value, offset and scale; ESPHome retains the room reading's tenths instead of the stock report's integer truncation. See [the formulas and reverse-engineering details](REVERSE_ENGINEERING.md#why-the-temperature-decoding-isnt-a-guess).

Physical louver positions behind the labels have not been checked on this unit. A fixed louver position disables swing on that axis; a swing range enables it; Default clears both position fields and disables swing.

**Outdoor temperature is an estimate, not a stock-verified conversion.** A raw reading of `35` coincided with an external sensor reading about `15.3 °C`, motivating a provisional offset of −20 °C and 1 °C per count (`35 → 15 °C`). The sensors were nearby, not colocated; one comparison cannot establish the scale or exact calibration. Compare at other outdoor temperatures before relying on it. The estimated sensor is enabled by default in the TAC-12CHSD profile (disabled by default in all-fields), updates every ten seconds, and reports unavailable for stale or too-short status replies. The TAC-12CHSD profile omits the raw outdoor-temperature entity; the all-fields profile exposes it disabled by default. Byte `P[30]` also remains visible in the AC receive logs. This byte does not establish tenths-of-a-degree precision. After OTA deployment on the tested unit, the encrypted API reported raw `35` and estimated `15 °C`; this verifies publication and the implemented subtraction, not the physical calibration.

`firmware/tac-12chsd-extras.yaml` also contains additional entities disabled by default and commented-out unsupported/extended features; `firmware/all-fields-extras.yaml` exposes those definitions for community testing. Decoding a property does not mean this AC implements it. Do not enable other-model features on the assumption that all TCL boards behave alike.

## Command behavior and limits

- Entity commands are queued and merged, then sent after fresh AC status. Unsent requests expire after five seconds. State is not published optimistically.
- No state-control command is sent automatically at boot or reconnect. Status is polled every two seconds after a 15-second startup delay; energy is polled automatically once synchronized.
- Scalar writes require an initial status synchronization, status no older than ten seconds, and a new status reply after each write. Every scalar command carries the complete cached state.
- All transmissions share at least 500 ms pacing. Direct API actions that hit the pacing guard are rejected rather than queued.
- Status payloads shorter than 25 bytes cannot synchronize, so controls remain refused. Frames must have valid checksums and the AC-to-module direction; loopback echoes do not unlock controls.
- Timer editing requires a complete 119-byte timer reply received within 30 seconds. The recorded TAC-12CHSD uses shorter status replies; timer support is not established on this unit.

These gates limit accidental commands; they do not establish electrical safety, appliance compatibility or long-term reliability. Full stock recovery remains untested.

### LibreTiny serial receive patch

Both control profiles wrap `SerialClass::read()` at link time ([`libretiny_serial_fix.h`](../firmware/libretiny_serial_fix.h)). The LibreTiny Realtek core drains the UART receive ring buffer from the main task without a critical section while the receive interrupt fills it; a single lost counter update shifts every later read (observed as a 5-byte shift that dropped the AC link on the tested unit). The wrapper holds the SDK's UART critical section for the duration of each read, which prevents that race.

## Logs

```sh
esphome logs firmware/tac-12chsd.yaml --device YOUR_DEVICE_IP
```

- `AC >>>`: bytes handed to the UART for transmission.
- `AC <<<`: raw received bytes; logging chunks are not frame boundaries.
- `RX checksum-valid`: valid checksum, **not** an acceptance acknowledgement.
- `RX status synchronized`: a supported status updated the cached state.

A successful API call or TX log does not prove acceptance. Compare returned status with the requested setting and observed behavior. Logs may include private network/device details; redact before sharing.

## Advanced API actions

Use Home Assistant entities for ordinary operation. The controls target also exposes these explicit actions:

| Action | Arguments / purpose |
| --- | --- |
| `ac_set` | `property` (string), `value` (float); validated scalar write |
| `ac_set_force` | Same arguments; bypasses freshness/post-write readback checks, **not** initial synchronization or pacing. Avoid for ordinary use |
| `ac_query` | `kind`: `status`, `auxiliary`, `energy` or `timers` |
| `ac_special_timer` | `on` (bool), `display`, `enable` (0/1), `minutes` (0–1439) |
| `ac_weekly_timer` | `bank` (1/2), `slot` (0–7), `display`, `enable`, `minutes`, `days` (digits 1–7; 7 is Sunday), `power_or_mode`, `fan`, `temperature`, `fahrenheit` |
| `ac_clear_timer_bank` | `bank` (1/2), `confirm=true`; clears that bank |
| `ac_set_clock` | `year` (2000–2099), `month`, `day`, `hour`, `minute`, `second`; local wall time, no implicit timezone conversion |
| `ac_clear_energy` | `confirm=true`; resets the AC's energy counters |

Weekly bank 1 uses `power_or_mode` as power 0/1. Bank 2 uses timer-specific numeric mode/fan encodings, **not** ordinary scalar enums. Timer editing preserves other slots; do not use it without understanding the received table. Factory reset, provisioning and arbitrary raw-packet actions are not exposed.

Scalar property names and validation are defined in [`tcl_ac.h`](../firmware/components/tcl_ac/tcl_ac.h) and [`ac_codec.h`](../firmware/ac_codec.h). Common values: `powerSwitch` 0/1; `targetTemperature` 16–31 in half-degree steps; `workMode` 0 auto, 1 cool, 2 dry, 3 fan, 4 heat; `windSpeed` 0 auto or 2–6 for Speed 1–5.

The optional helper reads your local API key. It requires the firmware Python environment. Without an action argument it only subscribes to logs and lists actions:

```sh
python -m tools.ac_control --host YOUR_DEVICE_IP
```

This next example **changes the appliance**, and is not an offline test:

```sh
python -m tools.ac_control --host YOUR_DEVICE_IP ac_set \
  '{"property":"targetTemperature","value":24.5}'
```
