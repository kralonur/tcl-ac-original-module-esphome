# Recover a WL6U module

**Start here if flashing, OTA or Wi-Fi went wrong.** Keep the original `stock.bin` and `stock.sha256` safe. Do not overwrite them with a read from a broken device.

> **What is proven:** entering the RTL8720CF ROM downloader, verified reads/writes, the five-region LibreTiny migration and normal ESPHome boot were recorded on the owner's unit. **A complete stock restore and stock AC operation after restoration have not been tested.** The full-backup restore below is a recovery procedure to verify, not a guaranteed factory-recovery claim.

## Pick your situation

| Symptom | First action |
| --- | --- |
| No Wi-Fi/API, but module boots | Check logs, credentials, DHCP and encryption key before writing anything |
| OTA failed but ESPHome still runs | Keep power stable; inspect the error and retry a known-good configuration |
| ESPHome will not boot / first installation was interrupted | Enter the serial ROM downloader; preserve a diagnostic read, then consider restoring your own full backup |
| Want the original TCL firmware/app back | Restore **your same-device pre-migration** 2 MiB stock backup; full functional restoration remains untested |
| No original backup | Stop. An ESPHome build cannot recreate stock credentials/calibration. Do not use someone else's dump |
| Downloader cannot connect | Troubleshoot wiring/strap/power/port first; erasing flash is not a connection fix |

All serial work below happens with the module **removed from and completely disconnected from the AC**. Unplug mains before removal. Never attach USB or bench power to an installed module.

## Wi-Fi recovery hotspot

`ac-controls.yaml` enables a password-protected **WL6U Recovery** hotspot after one minute without a Wi-Fi connection. This requires firmware built with the recovery configuration; older installations and the bench/debug/loopback targets do not provide it. Hotspot connection with the saved password, captive DNS, the recovery page and Wi-Fi reconfiguration were verified on the tested module on 2026-10-28. After entering the normal network credentials, the encrypted API and AC status/energy replies resumed.

1. Join **WL6U Recovery** from a phone or computer using the `recovery_ap_password` saved in your private `firmware/secrets.yaml`.
2. Open the captive portal, or browse to `http://192.168.4.1/` if it does not open automatically. Stay connected if the phone warns that the network has no internet.
3. Select or manually enter a compatible 2.4 GHz network and save its credentials. The network list was empty during the recorded test; manually entering the SSID worked. Reconnect Home Assistant using the device's new DHCP address if necessary; the API encryption key stays unchanged.

Keep the recovery password private: access to the portal permits Wi-Fi reconfiguration and firmware upload. Update the Wi-Fi credentials in the local secrets file too, so future builds use the correct network. This hotspot cannot recover firmware that no longer boots; use the serial route below if needed.

## Wi-Fi/API problems that may not need a restore

On the disconnected bench, remove the CLK strap for normal boot and monitor LOG-TX at **115200 8N1**. Use the [first-boot instructions](FLASHING.md#7-first-boot-on-the-bench); keep the monitor receive-only. Do not connect a serial terminal to the AC UART.

- No readable ESPHome boot: verify power, common ground, LOG-TX→adapter RXD, normal-boot strap state and the actual image installed.
- ESPHome boots but Wi-Fi does not: try the [recovery hotspot](#wi-fi-recovery-hotspot) if the installed controls firmware includes it. Otherwise check the credentials compiled into the image and compatible 2.4 GHz network access.
- Wi-Fi works but Home Assistant/API does not: check the actual DHCP IP, TCP reachability and the **API encryption key** used in your build. A new key may require reconfiguring the integration.
- No OTA endpoint: `bench.yaml`, `ac-debug.yaml` and `loopback.yaml` deliberately have none. The controls target has one. A serial recovery/installation is needed when the running image cannot accept OTA.
- No AC replies while disconnected: expected. Do not reinstall merely to troubleshoot bench boot or Wi-Fi.

If the installed firmware cannot join Wi-Fi and has no usable recovery hotspot, use the serial route below, restore stock if needed, then follow the installation guide again with corrected local secrets. Do not invent a factory-reset pin sequence.

## Cannot connect to the downloader

1. Power off. Recheck the [wiring table](FLASHING.md#2-wire-it-with-all-power-off): adapter TXD→**LOG-RX**, RXD←**LOG-TX**, shared GND. The connector's ordinary TX/RX are the wrong UART for programming.
2. Confirm module supply is its labeled **+5V input**; UART and CLK remain **3.3 V logic only**. Check for voltage drop, loose wires, TX/RX shorts or accidental dual power. Do not inject 5 V into chip pads.
3. Fit **CLK → 10 kΩ → verified 3.3 V while off**, then power on. Leave GPIO13/PA13 ungrounded and Reset/DATA/DTR/RTS unconnected. Do not pull CLK to GND.
4. Close all other serial applications. Select the actual adapter explicitly with `-d`; verify port permissions. Do not guess another connected device's port.
5. After a **fresh power-up**, link at `-B 115200 -b 460800`. After a successful command in that same download session, the next command links at `-B 460800 -b 460800`. A power-cycle resets this distinction. If unsure, power off and re-enter cleanly.
6. If communication still fails, stop and inspect the bench setup. Do not disable hash checking, ground random pads, run chip-wide erase commands or modify eFuse/OTP. ROM downloader failure does not prove the flash is irrecoverable, but this project does not provide a verified alternate SWD/BLE recovery route.

## Write failed or power was lost

**Do not connect the module to the AC or try to boot a half-migrated layout.** Keep the original backup and the installation plan/readbacks. A damaged flash bootloader does not itself erase the chip's ROM downloader; attempt the same CLK download entry above. Hardware damage, strap/power problems or security policy may still prevent access.

Once linked, first save a **new** full diagnostic read if communication is reliable. Do not resume by skipping to an assumed failed address or reconstruct the expected image from a partially changed flash. The simplest documented rollback is the full **same-device original stock backup** below. If any read/write/hash check fails, stop. No automatic retry or forced write is part of this guide.

## Restore the original stock backup (full restore is untested)

### 1. Check your backup before touching flash

Use the tools/environment from [flashing step 3](FLASHING.md#3-set-up-tools-and-choose-the-actual-port). From the project root, set the paths to your own retained backup:

```sh
. .venv/bin/activate
umask 077
PORT='/dev/serial/by-id/REPLACE_WITH_YOUR_ADAPTER'
BACKUP_DIR='../backups/REPLACE_WITH_YOUR_ORIGINAL_INSTALL_DIRECTORY'
BACKUP="$BACKUP_DIR/stock.bin"
(cd "$BACKUP_DIR" && sha256sum -c stock.sha256) && \
test "$(wc -c < "$BACKUP")" -eq 2097152 && \
echo 'Backup checksum and 2 MiB length verified.'
```

Require the final success message. Confirm from your records that this is **the same physical module's original stock backup**, not `expected-full.bin`, a generic build, a partial read, or a post-migration snapshot. A checksum proves file integrity, not which module it belongs to. If it is missing, corrupt or of uncertain origin, **stop**.

Do not use the owner's published whole-device hash as your restore target. It describes a different device's private data. The backup covers flash only (not eFuse/OTP), and this procedure never programs those.

### 2. Enter download mode and keep a diagnostic read

With all power off and the module disconnected from the AC, reconnect the [LOG wiring and CLK resistor](FLASHING.md#2-wire-it-with-all-power-off), then apply module power. Close all monitors:

```sh
LINK_BAUD=115200
ltchiptool flash info -d "$PORT" -B "$LINK_BAUD" -b 460800 -t 15 rtl8720c
```

Stop unless chip identity/security matches the recorded compatible target (RTL8720CF, VID 5/version 3, secure boot disabled). Do not change fuses. After the successful link, use 460800 for the existing session:

```sh
LINK_BAUD=460800
mkdir -p ../backups
RECOVERY="$(mktemp -d ../backups/recovery-XXXXXXXX)"
read_full() {
  ltchiptool flash read -d "$PORT" -B "$LINK_BAUD" -b 460800 -t 15 \
    -c -s 0 -l 0x200000 rtl8720c "$1"
}
read_full "$RECOVERY/before-restore.bin"
```

Use a new directory; never overwrite previous evidence. A full read can take about four minutes. If it fails, troubleshoot before writing.

### 3. Write your exact backup and verify the entire flash

**Destructive rollback:** this replaces the entire 2 MiB flash, including the migrated bootloader, settings and calibration regions, with this module's original bytes. It is not an ESPHome application update. Keep stable power and the CLK strap fitted; do not boot during the operation.

Run only after the checks above. The chain rechecks the original checksum and diagnostic-read size before writing:

```sh
(cd "$BACKUP_DIR" && sha256sum -c stock.sha256) && \
test "$(wc -c < "$BACKUP")" -eq 2097152 && \
test "$(wc -c < "$RECOVERY/before-restore.bin")" -eq 2097152 && \
ltchiptool flash write -d "$PORT" -B "$LINK_BAUD" -b 460800 -t 15 \
  -c -f rtl8720c -s 0x000000 -S 0 -l 0x200000 "$BACKUP" && \
read_full "$RECOVERY/after-restore.bin" && \
cmp "$BACKUP" "$RECOVERY/after-restore.bin" && \
echo 'All 2 MiB match the original backup. Power off before normal boot.'
```

Explicit `-f rtl8720c -s 0` selects the **raw write** path; `-S 0` means do not skip bytes in the backup. Do not substitute UF2, a generated flash image or a file downloaded from another device. Do not use `--no-check`. Raw writes do not require a separate erase command; the recorded ROM path erases sectors as it programs.

No final success message means the restore is **not verified**. Keep the module off the AC; retain all reads and errors privately. Do not repeatedly write to unstable hardware.

### 4. Check normal stock boot

After the exact **pre-boot** comparison succeeds:

1. Power off, remove the CLK resistor, and disconnect adapter TXD for receive-only logging while off.
2. Monitor LOG-TX at **115200 8N1**, then power on normally. Stock switches baud during startup; later TCL logs were readable at requested **921600 baud** with the owner's CP2102. Stop/reopen the monitor at that rate if needed. ESPHome logs use 115200, not the stock runtime rate.
3. Look for the recorded stock application/version and an active-image `md5 check success.`. This is a boot check, **not proof that stock cloud pairing or AC control is restored**. Do not assume old configuration-region warnings require more flashing; similar warnings existed before modification.
4. If boot fails, keep it on the bench. Save sanitized diagnostic information and seek review; do not fabricate partition fixes or use another module's credentials.
5. Only after satisfactory bench boot, power down, remove **all** programmer/USB/supply/strap wires, and reinstall in the original socket with AC mains off. Close the enclosure before restoring mains. Verify stock operation separately.

Normal boot may change userdata, so the full-flash comparison is intentionally done **before first boot**. Returning to stock does not guarantee cloud services or old pairing remain usable.

## What this guide cannot recover

- Lost original device credentials/calibration without a valid same-device backup.
- Hardware damage or a board with an unverified programming interface/security policy.
- Changed/burned eFuse/OTP settings; this project does not modify them.
- Guaranteed stock functional behavior: a complete restore has not yet been recorded here.

For help, share the exact module/revision, failing command and a **redacted** error, not a dump, firmware binary, MAC/serial, key or unredacted log. Keep private files out of public issues and attachments.
