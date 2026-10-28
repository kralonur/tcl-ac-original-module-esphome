# Inside the original WL6U firmware

The goal was to keep the working Wi-Fi module instead of replacing it with an ESP32. That required understanding what its firmware does between the network and the indoor unit.

Started with a flash dump read directly from a WL6U / RTL8720CF module running stock application `V8-R82CT20-WFMV205.11.3.27`. Disassembled the application and traced its command-building, UART, status-decoding and reporting code. This was analysis of the firmware on the chip, not phone-app traffic sniffing or a recreation of the cloud API.

These notes describe that application and the tested TAC-12CHSD. They aren't a claim that every TCL model uses the same protocol or supports every property below.

## Checking the decoding

Reading disassembly was only the first step. Selected functions from the original application were also run locally with Unicorn, an emulator, and their output compared with the replacement codecs.

The original instructions handled the byte packing and arithmetic; surrounding services such as allocation, logging, clocks and JSON callbacks were stubbed. That allowed individual functions to be checked without running the whole cloud-connected application or sending commands to the AC.

The checks included:

- All 89 slots in the bounded scalar setter dispatch table, including unnamed cases and default returns.
- All 56 named numeric properties through the stock property-node builder.
- 504 single-bit status inputs (63 bytes × 8), followed by another 504 through the checksum/change-detection path.
- Temperature and numeric encodings, error-bit mapping, timer records and date/time frames.
- Split frames, consecutive frames, invalid headers, length boundaries and checksum rejection.

After the offline work, the replacement was tested on the TAC-12CHSD. In the recorded scalar-command runs, 340 transmitted control frames matched the host codec byte for byte. Status replies confirmed the settings that the AC reports back. That doesn't prove an audible beep, a physical louver position or every possible feature worked.

The private dump isn't distributed.

## What came out of the dump

### The module-to-AC protocol

The AC connection uses PA11 TX / PA12 RX at 9600 baud, eight data bits, even parity and one stop bit. It is separate from the module's programming/log UART.

Frames start with `BB`, followed by direction bytes, a command, a payload length, the payload and an XOR checksum. Module-to-AC frames use `BB 00 01`; replies use `BB 01 00`. XOR across the complete valid frame is zero.

Traced the ordinary control/status path, auxiliary and energy queries, timer messages and clock updates. A scalar command sends the whole cached state, not just one changed property. The replacement therefore reads status before constructing commands and preserves unrelated settings.

Transmit and receive layouts differ. Some mode and fan values also use different numeric mappings in each direction, so copying incoming bytes straight into an outgoing command would be wrong.

### The controls behind the app

The stock property table contains 56 names covering 52 distinct IDs. Of those, 54 names / 50 IDs select ordinary state-changing setters; two named IDs return without changing state. The dispatch table also contains unnamed cases, including louver subfields and factory/provisioning operations.

The useful control families include:

- Power, operating mode, target temperature and temperature-unit flags.
- Fan speed, quiet mode, Eco, Boost and Sleep.
- Vertical and horizontal swing, fixed positions and separate left/right fields.
- Display, beep, health, anti-mould, light sensing and 3D airflow.
- Additional stock properties for heating, cleaning, filtering, dual-zone operation and other model-specific features.

A name in that table is not proof of a working feature on the tested AC. The normal Home Assistant entities cover the everyday controls. Additional and model-specific definitions live in [`ac-extras.yaml`](../firmware/ac-extras.yaml): some are available but disabled by default, while unsupported or extended features remain commented out. Factory and provisioning setters are not exposed as ordinary controls.

The complete implemented names and validation are in [`tcl_ac.h`](../firmware/components/tcl_ac/tcl_ac.h) and [`ac_codec.h`](../firmware/ac_codec.h). See [controls](CONTROLS.md) for entity behavior and API actions.

## Why the temperature decoding isn't a guess

The formulas below come from the stock firmware's own packing and reporting functions. `P` means the received status payload, starting after the five-byte frame header; indices start at zero.

### Room temperature

The room-temperature reading is a packed ten-bit value, not a single byte:

```text
raw = (P[12] << 2) | ((P[13] >> 2) & 3)
room_temperature_c = (raw - 200) / 10.0
```

The original firmware's `currentTemperature` report uses the same packed value and offset but performs signed integer division:

```text
stock_report = trunc((raw - 200) / 10)
```

That discards the fractional part. The ESPHome climate entity and room-temperature sensor keep it. For example, a raw value of `453` gives **25.3 °C** here, while the stock reporting function produces **25**.

So the offset and scale aren't adjustments made to get a plausible reading: they are taken from the original firmware. Keeping tenths preserves information already present in the AC's reply. It does not make the physical sensor more accurate, calibrate it against a thermometer or prove what the phone app displays in every version.

The packed-value and stock-compatible integer helpers are in [`ac_report.h`](../firmware/ac_report.h). The climate conversion is in [`tcl_ac_climate.h`](../firmware/components/tcl_ac/tcl_ac_climate.h), and the separate sensor uses the same conversion in [`ac-controls.yaml`](../firmware/ac-controls.yaml).

### Target temperature

The received setpoint uses different fields:

```text
unit_flag = (P[5] >> 7) & 1
half_bit = (P[4] >> (unit_flag ? 0 : 1)) & 1
target_temperature = (P[3] & 0x0F) + 16 + 0.5 * half_bit
```

The stock expression selects a different half-degree bit according to the unit flag; it does not perform a Celsius/Fahrenheit conversion here. The ordinary configuration uses Celsius.

Outgoing Celsius commands use an inverted integer nibble, `(31 - whole_degrees) & 0x0F`, plus a separate fractional flag. The stock encoder sets that flag when the fractional part exceeds `0.1`; it isn't a general rounding function. The replacement restricts normal setpoints to 16–31 °C in 0.5 °C steps rather than accepting arbitrary fractions or reproducing unchecked stock inputs.

Those half-degree setpoints were also sent and read back on the tested AC across the supported range.

## Getting past the app's view of the AC

Access to received data is no longer limited to what the app displays. ESPHome reads the AC's replies directly and makes the decoded information available locally. Raw receive logs also expose bytes that don't yet have a reliable interpretation.

| Information | How it is exposed / what is known |
| --- | --- |
| Power, mode, setpoint, fan, presets and swing | Normal Home Assistant controls and returned status |
| Room temperature | Stock-derived conversion, preserving tenths |
| Display, health, anti-mould, light sense and related flags | Decoded switches where supported |
| Error flags | Numeric codes mapped from the stock reporting code, not guessed fault descriptions |
| Energy replies | Decoded counter, raw values and derived power; see the unit caveat below |
| AC type, payload length, filter flag and additional state | Optional diagnostic entities |
| Extra hardware bytes | Optional raw entities, including bytes tentatively labelled coil, fan, outdoor and discharge readings |
| Extended properties and timers | Implemented decoding/actions, usable only when the AC supplies the required fields |
| Remaining received bytes | Raw UART receive logs, not invented sensor values |

This is access to the AC-to-module data rather than only the app's selected presentation. It is **not** a promise that every byte becomes a named Home Assistant entity, that every internal AC measurement is sent over this connection, or that undocumented commands outside this Wi-Fi firmware have been discovered.

There are still important gaps:

- The stock firmware doesn't decode some hardware bytes. Labels such as supply voltage and compressor frequency are inferred; other hardware fields deliberately stay raw.
- The original energy-counter arithmetic was recovered, but its physical unit wasn't established by that alone. The current configuration presents kWh and derives power from counter changes; those units still need independent measurement validation.
- Error numbers and capability IDs don't automatically reveal their physical meaning.
- Beep is the module's remembered requested value, not a state returned by the AC.
- The tested unit returns a 55-byte status payload. Some stock fields require bytes through index 62 and aren't available on this unit.

## Timers and clock

The stock code contains special ON/OFF timers, two weekly banks with eight slots each, and date/time message encoding. The full timer payload is 119 bytes and includes fields that need to be preserved when changing a slot.

The tested AC answers the timer query with a short reply, not that full table. Timer writes therefore stay refused on this unit. Decoding the format wasn't enough to make the hardware support it.

Where a compatible unit does return the full table, the replacement validates slot indices, times and other inputs, and preserves other slots when editing one. That deliberately differs from the stock weekly-array update, which clears a bank before applying its supplied records.

## What this work does and doesn't establish

The strongest evidence is a combination of stock instructions, emulator comparisons and matching live replies. Each answers a different question: what the original code calculates, whether the replacement matches it, and what this particular AC reports.

None of those alone proves physical sensor accuracy, support on other models, long-term reliability or safe electrical access. The firmware dump and credentials remain private. No vendor firmware is included in this repository.

The byte-level calculations described here were checked against the original module firmware.
