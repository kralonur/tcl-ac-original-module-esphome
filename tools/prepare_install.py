#!/usr/bin/env python3
"""Prepare a WL6U first-install plan offline. Never opens a port or flashes hardware."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from tools.stock_layout import (
    FLASH_SIZE,
    check, parse_tcl_image, parse_tcl_table,
)

BOARD = "generic-rtl8720cf-2mb-992k"
REGIONS = {"part_table": (0, 0x1000), "boot": (0x4000, 0x8000),
           "ota1": (0xC000, 0xF8000), "ota2": (0x104000, 0xF8000),
           "kvs": (0x1FC000, 0x4000)}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def validate_stock(stock):
    check(len(stock) == FLASH_SIZE, "Backup must be exactly 2,097,152 bytes")
    partitions = parse_tcl_table(stock)
    image = parse_tcl_image(stock)
    check(partitions == {
        "/system": {"start": 0x17F00, "length": 0x100100},
        "/systembackup": {"start": 0x118000, "length": 0xD0000},
        "/userdata": {"start": 0x1E8000, "length": 0x18000},
        "/userbootdata": {"start": 0x12000, "length": 0x1000},
        "/prodcfgdata": {"start": 0x13000, "length": 0x4000},
    } and image["start"] == 0x18000, "Unrecognized stock layout; stop for review")


def load_build(build):
    # ltchiptool's parser verifies image hashes and the application sum32 offline.
    from ltchiptool.soc.ambz2.util.models.images import Flash

    board = json.loads((build / "board.json").read_text())
    check(board["build"]["variant"] == BOARD, "Wrong build board")
    for name, region in REGIONS.items():
        actual = tuple(int(n, 0) for n in board["flash"][name].split("+"))
        check(actual == region, f"Wrong build region: {name}")
    raw = (build / "image_flash_is.0x000000.bin").read_bytes()
    key = bytes.fromhex(board["image"]["keys"]["hash_keys"]["part_table"])
    parsed = Flash.unpack(raw, hash_key=key)
    check([(p.type.name, p.offset, p.length) for p in parsed.ptable.data.partitions] ==
          [("BOOT", *REGIONS["boot"]), ("FW1", *REGIONS["ota1"]),
           ("FW2", *REGIONS["ota2"])], "Unexpected generated partition table")
    parts = []
    for name, filename in (
        ("ota1", "image_firmware_is.0x00C000.bin"),
        ("boot", "image_bootloader.0x004000.bin"),
        ("part_table", "image_part_table.0x000000.bin"),
    ):
        data = (build / filename).read_bytes()
        start, limit = REGIONS[name]
        check(0 < len(data) <= limit, f"Empty or oversized {filename}")
        expected = raw[start:start + limit]
        if name != "ota1":
            # Match ltchiptool's boot/table output: trim FF, then align to 32 bytes.
            expected = expected.rstrip(b"\xff")
            expected += b"\xff" * (-len(expected) % 0x20)
        else:
            check(len(raw) <= start + limit, "Generated application exceeds its region")
        check(data == expected, f"Mismatched or truncated build part: {filename}")
        parts.append(data)
    return parts


def make_plan(stock, application, bootloader, table):
    check(len(stock) == FLASH_SIZE, "Backup must be exactly 2,097,152 bytes")
    check(table[:16] == stock[:16], "Flash calibration magic differs; stop for review")
    writes = []
    for name, start, data, limit in (
        ("01-application.bin", 0xC000, application, 0xF8000),
        ("02-ota2-header.bin", 0x104000, b"\xff" * 0x1000, 0x1000),
        ("03-kvs.bin", 0x1FC000, b"\xff" * 0x4000, 0x4000),
        ("04-bootloader.bin", 0x4000, bootloader, 0x8000),
        ("05-partition-table.bin", 0, table, 0x1000),
    ):
        check(0 < len(data) <= limit, f"Empty or oversized {name}")
        data += b"\xff" * (-len(data) % 0x1000)
        writes.append((name, start, data))
    expected = bytearray(stock)
    for _, start, data in writes:
        expected[start:start + len(data)] = data
    check(expected[0x1000:0x4000] == stock[0x1000:0x4000], "System reservation changed")
    return writes, bytes(expected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backup", type=Path, required=True, help="Verified full stock backup from this device")
    parser.add_argument("--build-dir", type=Path, required=True, help="ESPHome .pioenvs/tcl-wl6u-ac directory")
    parser.add_argument("--output", type=Path, required=True, help="New private directory; must not exist")
    args = parser.parse_args()
    try:
        stock = args.backup.read_bytes()
        validate_stock(stock)
        writes, expected = make_plan(stock, *load_build(args.build_dir))
        args.output.mkdir(mode=0o700, parents=True, exist_ok=False)
        files = {name: data for name, _, data in writes}
        files["expected-full.bin"] = expected
        manifest = {"board": BOARD, "backup_sha256": sha256(stock),
                    "expected_sha256": sha256(expected),
                    "writes": [{"file": name, "start": start, "length": len(data),
                                "sha256": sha256(data)} for name, start, data in writes]}
        files["plan.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
        for name, data in files.items():
            with (args.output / name).open("xb") as stream:
                (args.output / name).chmod(0o600)
                stream.write(data)
    except (OSError, ValueError, KeyError, ImportError, struct.error) as exc:
        parser.exit(1, f"Preparation refused: {exc}\n")
    print(f"Prepared {args.output}; no hardware accessed. Follow docs/FLASHING.md before writing.")


if __name__ == "__main__":
    main()
