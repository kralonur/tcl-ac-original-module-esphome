# Home Assistant controls

Add the device through Home Assistant's **ESPHome integration**, using its address and the API encryption key from your local secrets. No MQTT broker is needed.

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
| Sleep level / Generator mode | Numeric controls; supported ranges are defined in the YAML |
| Room temperature | Received temperature in tenths of a degree Celsius |
| Power / Energy | Power derived from energy-counter changes; cumulative energy in kWh |
| Supply voltage / Compressor frequency | **Inferred labels**, not verified measurements; voltage byte can wrap above 255 |
| AC link | Status received within the last ten seconds |
| Error codes | Received numeric codes or `none`; not a diagnosis |

Temperature decoding uses the original firmware's packed value, offset and scale; ESPHome retains the room reading's tenths instead of the stock report's integer truncation. See [the formulas and reverse-engineering details](REVERSE_ENGINEERING.md#why-the-temperature-decoding-isnt-a-guess).

Physical louver positions behind the labels have not been checked on this unit. A fixed louver position disables swing on that axis; a swing range enables it; Default clears both position fields and disables swing.

`firmware/ac-extras.yaml` contains additional entities disabled by default and commented-out unsupported/extended features. Decoding a property does not mean this AC implements it. Do not enable other-model features on the assumption that all TCL boards behave alike.

## Command behavior and limits

- Entity commands are queued and merged, then sent after fresh AC status. Unsent requests expire after five seconds. State is not published optimistically.
- No state-control command is sent automatically at boot or reconnect. Status is polled every two seconds after a 15-second startup delay; energy is polled automatically once synchronized.
- Scalar writes require an initial status synchronization, status no older than ten seconds, and a new status reply after each write. Every scalar command carries the complete cached state.
- All transmissions share at least 500 ms pacing. Direct API actions that hit the pacing guard are rejected rather than queued.
- Status payloads shorter than 25 bytes cannot synchronize, so controls remain refused. Frames must have valid checksums and the AC-to-module direction; loopback echoes do not unlock controls.
- Timer editing requires a complete 119-byte timer reply received within 30 seconds. The recorded TAC-12CHSD uses shorter status replies; timer support is not established on this unit.

These gates limit accidental commands; they do not establish electrical safety, appliance compatibility or long-term reliability. Full stock recovery remains untested.

## Logs

```sh
esphome logs firmware/ac-controls.yaml --device YOUR_DEVICE_IP
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
