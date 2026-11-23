#!/usr/bin/env python3
"""Provision Wi-Fi and the XAI Router API key into an AI Passport running ChatGPT.

The device exposes a console on its native USB Serial/JTAG port. This script
sends `wifi`, `key`, and optional `host`/`voice`/`prompt` commands and waits for
each "OK <cmd>" reply. Secrets are read from environment variables or typed at
a hidden prompt; they are never written to disk or printed.

    python3 tools/live_provision.py --ssid MyWiFi
    XAI_API_KEY=sk-... WIFI_PASSWORD=... python3 tools/live_provision.py --ssid MyWiFi

Requires pyserial (available in the activated ESP-IDF Python environment).
Writing the merged full.bin at 0x0 erases these settings; run this again afterwards.
"""

from __future__ import annotations

import argparse
import getpass
import os
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover - depends on the host environment
    print("pyserial is required: activate ESP-IDF (. $IDF_PATH/export.sh) or pip install pyserial",
          file=sys.stderr)
    sys.exit(2)

ESPRESSIF_VID = 0x303A


def find_port() -> str:
    candidates = [p.device for p in list_ports.comports() if p.vid == ESPRESSIF_VID]
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        sys.exit("No Espressif USB Serial/JTAG device found. Connect the AI Passport with a data "
                 "cable and power it on, or pass --port.")
    sys.exit(f"Several Espressif devices found ({', '.join(candidates)}); choose one with --port.")


def quote(value: str) -> str:
    """Quote one argument for esp_console's argv splitter."""
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


class Console:
    def __init__(self, port: str) -> None:
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.2
        # Keep DTR/RTS deasserted so opening the port does not reset the chip.
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.buffer = b""

    def close(self) -> None:
        self.ser.close()

    def _lines(self, deadline: float):
        while time.monotonic() < deadline:
            chunk = self.ser.read(256)
            if chunk:
                self.buffer += chunk
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                yield line.decode("utf-8", "replace").strip()

    def sync(self) -> None:
        for _ in range(5):
            self.ser.write(b"\r\n")
            for line in self._lines(time.monotonic() + 1.0):
                if line.startswith("passport>") or "OK config" in line:
                    return
            self.ser.write(b"config\r\n")
            for line in self._lines(time.monotonic() + 2.0):
                if "OK config" in line:
                    return
        sys.exit("The device console did not answer. Is the ChatGPT firmware running?")

    def run(self, name: str, line: str, secret: bool = False, timeout: float = 6.0) -> list[str]:
        self.buffer = b""
        self.ser.reset_input_buffer()
        self.ser.write(line.encode("utf-8") + b"\r\n")
        output: list[str] = []
        for text in self._lines(time.monotonic() + timeout):
            if secret and text.startswith(("passport>", name + " ", "wifi ", "key ")):
                continue  # do not keep the echoed command line containing a secret
            if text.startswith(f"OK {name}"):
                return output
            if text.startswith(f"ERR {name}"):
                sys.exit(f"Device rejected `{name}`: {text}")
            output.append(text)
        sys.exit(f"Timed out waiting for `{name}` to complete.")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: the single Espressif USB device)")
    ap.add_argument("--ssid", help="Wi-Fi SSID (2.4 GHz)")
    ap.add_argument("--open-network", action="store_true", help="the Wi-Fi network has no password")
    ap.add_argument("--host", help="API host, default api.xairouter.com")
    ap.add_argument("--voice", help="output voice, default marin")
    ap.add_argument("--prompt", help="assistant instructions")
    ap.add_argument("--skip-key", action="store_true", help="keep the stored API key")
    ap.add_argument("--show", action="store_true", help="only print the current (masked) settings")
    args = ap.parse_args()

    port = args.port or find_port()
    console = Console(port)
    try:
        console.sync()
        if not args.show:
            if args.ssid:
                password = "" if args.open_network else os.environ.get("WIFI_PASSWORD")
                if password is None:
                    password = getpass.getpass(f"Wi-Fi password for {args.ssid}: ")
                cmd = f"wifi {quote(args.ssid)}" + (f" {quote(password)}" if password else "")
                console.run("wifi", cmd, secret=True)
                print("Wi-Fi saved.")
            if not args.skip_key:
                key = os.environ.get("XAI_API_KEY") or getpass.getpass("XAI Router API key: ")
                console.run("key", f"key {quote(key.strip())}", secret=True)
                print("API key saved.")
            if args.host:
                console.run("host", f"host {quote(args.host)}")
            if args.voice:
                console.run("voice", f"voice {quote(args.voice)}")
            if args.prompt is not None:
                console.run("prompt", f"prompt {quote(args.prompt)}")
        for line in console.run("config", "config"):
            if "=" in line:
                print("  " + line)
    finally:
        console.close()
    print("Done. The device reconnects automatically; press OK on it to start talking.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
