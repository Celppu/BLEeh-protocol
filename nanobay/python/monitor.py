#!/usr/bin/env python3
"""Show all 16 CRSF channels over custom BLE GATT, plus normal BLE HID gamepad.

  python monitor.py              # BLE scan + GUI
  python monitor.py --demo       # GUI with simulated stick movement, no hardware
  python monitor.py --address ADDRESS
  python monitor.py --list       # list discovered BLE addresses/names

Python 3.10+, `pip install -r requirements.txt`; Tkinter is bundled with
most desktop Python installs (Linux sometimes needs python3-tk).
"""
import argparse
import asyncio
import collections
import math
import queue
import threading
import time
import tkinter as tk
from tkinter import ttk
from protocol import (DEVICE_NAME, SERVICE_UUID, CHAR_UUID, ChannelAssembler,
                      ChannelFrame, CRSF_MIN, CRSF_MID, CRSF_MAX, normalized)


async def ble_worker(events, stop, address=None, scan_only=False):
    from bleak import BleakClient, BleakScanner
    if scan_only:
        print("Scanning for 8 seconds...")
        for device in await BleakScanner.discover(timeout=8):
            print(f"{device.address:22} {device.name}")
        return
    while not stop.is_set():
        try:
            events.put(("status", "Scanning for Pocket BLE Bridge..."))
            target = address
            if not target:
                target = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10)
                if target is None:
                    raise RuntimeError("Device not found; use BOOT x3 to enter pairing mode")
            events.put(("status", "Connecting..."))
            async with BleakClient(target, timeout=20) as client:
                events.put(("status", f"Connected to {DEVICE_NAME}; subscribing..."))
                assembler = ChannelAssembler()

                def got_packet(_characteristic, data: bytearray):
                    try:
                        frame = assembler.feed(bytes(data))
                        if frame:
                            events.put(("frame", frame))
                    except ValueError as exc:
                        events.put(("status", f"Bad packet: {exc}"))

                await client.start_notify(CHAR_UUID, got_packet)
                events.put(("status", "Connected: 16-channel GATT stream active"))
                while client.is_connected and not stop.is_set():
                    await asyncio.sleep(0.15)
        except Exception as exc:
            events.put(("status", f"BLE: {exc}; retrying..."))
        if not stop.is_set():
            await asyncio.sleep(2)


class Monitor:
    def __init__(self, root, events, stop):
        self.root, self.events, self.stop = root, events, stop
        self.status = tk.StringVar(value="Starting...")
        self.header = tk.StringVar(value="No channel data yet")
        root.title("RadioMaster Pocket — 16 BLE CRSF Channels")
        root.geometry("740x720")
        root.minsize(620, 630)
        outer = ttk.Frame(root, padding=14)
        outer.pack(fill="both", expand=True)
        ttk.Label(outer, text="Pocket BLE Bridge / 16 channels",
                  font=("TkDefaultFont", 17, "bold")).pack(anchor="w")
        ttk.Label(outer, textvariable=self.status, wraplength=680).pack(anchor="w", pady=(5, 2))
        ttk.Label(outer, textvariable=self.header).pack(anchor="w")
        self.rate = tk.StringVar(value="Rate: --")
        ttk.Label(outer, textvariable=self.rate,
                  font=("TkDefaultFont", 12, "bold")).pack(anchor="w", pady=(2, 12))
        self.bars, self.labels = [], []
        for index in range(16):
            row = ttk.Frame(outer)
            row.pack(fill="x", pady=3)
            ttk.Label(row, text=f"CH{index+1:02}", width=6).pack(side="left")
            bar = ttk.Progressbar(row, orient="horizontal", maximum=100)
            bar.pack(side="left", fill="x", expand=True, padx=(5, 14))
            val = ttk.Label(row, text="---", width=19)
            val.pack(side="right")
            self.bars.append(bar)
            self.labels.append(val)
        ttk.Label(outer,
                  text="Raw CRSF: ~172=min, 992=center, ~1811=max. "
                       "BLE HID exposes CH1–8 as axes; CH9–16 also appear as buttons.",
                  wraplength=700).pack(anchor="w", pady=(14, 0))
        self.last_packet_at = 0.0
        # Rolling 1 s window of complete 16-channel frames for the Hz display.
        self.frame_times = collections.deque()
        self.last_seq = None
        self.missing_in_window = collections.deque()  # (time, missing count)
        self.root.after(35, self.pump)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def pump(self):
        latest = None
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "status":
                    self.status.set(value)
                elif kind == "frame":
                    self.count(value)
                    latest = value
        except queue.Empty:
            pass
        # Count every frame but redraw only the newest; Tk cannot keep up with
        # redrawing 16 bars per frame at 125 Hz.
        if latest is not None:
            self.show(latest)
        self.update_rate()
        if self.last_packet_at and time.monotonic() - self.last_packet_at > 0.8:
            self.header.set("NO FRESH BLE PACKETS — receiver must fail safe")
        if not self.stop.is_set():
            self.root.after(35, self.pump)

    def count(self, frame: ChannelFrame):
        now = time.monotonic()
        self.frame_times.append(now)
        if self.last_seq is not None:
            gap = (frame.sequence - self.last_seq - 1) % 65536
            if 0 < gap < 1000:  # ignore reconnects/firmware restarts
                self.missing_in_window.append((now, gap))
        self.last_seq = frame.sequence

    def update_rate(self):
        cutoff = time.monotonic() - 1.0
        while self.frame_times and self.frame_times[0] < cutoff:
            self.frame_times.popleft()
        while self.missing_in_window and self.missing_in_window[0][0] < cutoff:
            self.missing_in_window.popleft()
        missing = sum(n for _, n in self.missing_in_window)
        self.rate.set(f"Rate: {len(self.frame_times)} Hz (16-ch frames/s)  |  "
                      f"missing: {missing}/s")

    def show(self, frame: ChannelFrame):
        self.last_packet_at = time.monotonic()
        self.header.set(f"Sequence {frame.sequence:05d}  |  "
                        f"CRSF {'OK' if frame.fresh else 'STALE / NEUTRAL'}  |  "
                        f"Pair mode {'YES' if frame.pairing else 'no'}")
        for i, raw in enumerate(frame.channels):
            self.bars[i]["value"] = max(0, min(100,
                     (raw - CRSF_MIN) * 100 / (CRSF_MAX - CRSF_MIN)))
            self.labels[i]["text"] = f"{raw:4d}   {normalized(raw):+0.2f}"

    def close(self):
        self.stop.set()
        self.root.destroy()


def demo_worker(events, stop):
    step = 0
    events.put(("status", "DEMO MODE (no BLE connection)"))
    while not stop.is_set():
        channel_values = tuple(max(CRSF_MIN, min(CRSF_MAX,
                            round(CRSF_MID + 790 * math.sin(step / 11 + i / 2))))
                               for i in range(16))
        events.put(("frame", ChannelFrame(step % 65536, channel_values, True, False)))
        step += 1
        time.sleep(0.04)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--demo", action="store_true", help="generate fake channel values")
    ap.add_argument("--list", action="store_true", help="list nearby BLE devices and exit")
    ap.add_argument("--address", help="address/UUID passed directly to Bleak")
    args = ap.parse_args()
    if args.list:
        asyncio.run(ble_worker(queue.Queue(), threading.Event(), scan_only=True))
        return
    events, stop = queue.Queue(), threading.Event()
    target = ((lambda: demo_worker(events, stop)) if args.demo
              else (lambda: asyncio.run(ble_worker(events, stop, args.address))))
    threading.Thread(target=target, daemon=True).start()
    root = tk.Tk()
    Monitor(root, events, stop)
    root.mainloop()


if __name__ == "__main__":
    main()
