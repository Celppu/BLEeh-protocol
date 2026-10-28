#!/usr/bin/env python3
"""Use a PC as a BLEeh transmitter to test a receiver without a radio.

  python bleeh_tx.py --scan                 # list BLEeh receivers nearby
  python bleeh_tx.py --pair                 # bond the receiver that is in pairing mode
  python bleeh_tx.py --address AA:BB:...    # connect to an already bonded receiver
  python bleeh_tx.py --address ... --sweep  # move CH1..CH4 slowly

CH5 (arm) is held LOW unless you pass --arm. Lift wheels / remove props first.
Requires: pip install -r requirements.txt (bleak).
"""
import argparse
import asyncio
import math
import time

from bleak import BleakClient, BleakScanner

import bleeh as ll


def describe(adv):
    for cid, payload in adv.manufacturer_data.items():
        info = ll.decode_adv_manufacturer(cid, payload)
        if info:
            return info
    return None


async def scan(timeout=5.0):
    found = []
    devices = await BleakScanner.discover(timeout=timeout, return_adv=True)
    for dev, adv in devices.values():
        if ll.SERVICE_UUID not in [u.lower() for u in adv.service_uuids]:
            continue
        info = describe(adv)
        pairing = bool(info and info[1] & ll.ADV_FLAG_PAIRING)
        found.append((dev, adv, pairing))
        print(f"{dev.address:20} {adv.local_name or dev.name or '?':20} "
              f"RSSI {adv.rssi:4} dBm  {'PAIRING' if pairing else ''}")
    return found


def on_telemetry(_, data: bytearray):
    t = ll.decode_telemetry(bytes(data))
    parts = []
    if t.link:
        parts.append(f"LQ {t.link['uplink_lq']:3}% RSSI {t.link['uplink_rssi']:4} dBm"
                     f"{' FAILSAFE' if t.link['failsafe'] else ''}")
    if t.battery:
        parts.append(f"BAT {t.battery['millivolts'] / 1000:.2f} V {t.battery['remaining_pct']}%")
    if t.flight_mode:
        parts.append(f"MODE {t.flight_mode}")
    print(f"telemetry #{t.sequence:3}: " + " | ".join(parts))


async def run(args):
    target = args.address
    if not target:
        print("Scanning...")
        found = [f for f in await scan() if f[2] or not args.pair]
        if not found:
            print("No receiver found" + (" in pairing mode (triple-click its button)" if args.pair else ""))
            return
        target = max(found, key=lambda f: f[1].rssi)[0]

    async with BleakClient(target) as client:
        if args.pair:
            await client.pair()  # Just Works bond; the receiver must be in pairing mode
            print("Bonded")
        caps = ll.decode_capabilities(await client.read_gatt_char(ll.CAPS_UUID))
        print(f"Receiver '{caps.name}' protocol v{caps.protocol_version}, "
              f"{caps.channel_count} channels, features 0x{caps.features:08x}")
        await client.start_notify(ll.TELEMETRY_UUID, on_telemetry)

        seq, period, t0 = 0, 1.0 / args.rate, time.monotonic()
        print(f"Sending RC at {args.rate} Hz, Ctrl+C to stop")
        while client.is_connected:
            channels = [ll.CHANNEL_MID] * ll.NUM_CHANNELS
            if args.sweep:
                for i in range(4):
                    channels[i] = int(ll.CHANNEL_MID + 600 * math.sin((time.monotonic() - t0) + i))
            channels[4] = ll.CHANNEL_MAX if args.arm else ll.CHANNEL_MIN
            await client.write_gatt_char(ll.RC_UUID, ll.encode_rc(channels, seq), response=False)
            seq = (seq + 1) & 0xFFFF
            await asyncio.sleep(period)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scan", action="store_true", help="list BLEeh receivers and exit")
    ap.add_argument("--pair", action="store_true", help="bond a receiver that is in pairing mode")
    ap.add_argument("--address", help="receiver address (Windows/Linux MAC, macOS UUID)")
    ap.add_argument("--rate", type=float, default=50, help="RC frames per second (default 50)")
    ap.add_argument("--sweep", action="store_true", help="move CH1..CH4 in slow sine waves")
    ap.add_argument("--arm", action="store_true", help="hold CH5 high (motors may run!)")
    args = ap.parse_args()
    try:
        asyncio.run(scan() if args.scan else run(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
