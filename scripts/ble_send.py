#!/usr/bin/env python3
"""Send one test packet to the Flight Radar device over BLE (sub-project A harness).

Usage: pip install bleak; python3 scripts/ble_send.py
On macOS, the terminal app needs Bluetooth permission (System Settings > Privacy).
Mirror of the wire format in src/ble_core.h.
"""
import asyncio
import struct
from bleak import BleakScanner, BleakClient

DEVICE_NAME = "FlightRadar"
CHAR_UUID   = "f1a90002-7e1d-4c2a-9b3f-1a2b3c4d5e6f"

FLAG_GROUND, FLAG_ALT_VALID, FLAG_GS_VALID = 0x01, 0x02, 0x04
FLAG_TRACK_VALID, FLAG_SQUAWK_VALID = 0x08, 0x10


def _field(s: str, n: int) -> bytes:
    b = s.encode("ascii", "ignore")[:n]
    return b + b" " * (n - len(b))


def _record(cs, ty, lat, lon, alt_ft, gs_kt, flags, track=0, squawk=0) -> bytes:
    return (_field(cs, 8) + _field(ty, 4)
            + struct.pack("<ffihBB", lat, lon, alt_ft, gs_kt, flags, 0)
            + struct.pack("<hH", track, squawk))


def _packet(clat, clon, aircraft) -> bytes:
    pkt = struct.pack("<BBBB", 0x46, 0x52, 2, len(aircraft))  # 'F','R',version,count
    pkt += struct.pack("<ff", clat, clon)
    for a in aircraft:
        pkt += _record(*a)
    return pkt


async def main():
    dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10)
    if not dev:
        print(f"device '{DEVICE_NAME}' not found"); return
    async with BleakClient(dev) as client:
        clat, clon = 38.7677, -9.3006  # Lisbon-ish center
        aircraft = [
            ("RYR4KP", "B738", 38.80, -9.28, 12000, 420,
             FLAG_ALT_VALID | FLAG_GS_VALID | FLAG_TRACK_VALID | FLAG_SQUAWK_VALID, 270, 1200),
            ("EMERG1", "A320", 38.72, -9.40, 35000, 450,
             FLAG_ALT_VALID | FLAG_GS_VALID | FLAG_TRACK_VALID | FLAG_SQUAWK_VALID, 90, 7700),
            ("ABC123", "B772", 38.70, -9.10, 0, 5, FLAG_GROUND, 0, 0),
        ]
        # Write WITH response: verified on-device, and a long write (prepared) reliably
        # carries the full packet (up to BLE_MAX_PACKET) regardless of negotiated MTU.
        await client.write_gatt_char(CHAR_UUID, _packet(clat, clon, aircraft), response=True)
        print(f"sent {len(aircraft)} aircraft to {DEVICE_NAME}")


if __name__ == "__main__":
    asyncio.run(main())
