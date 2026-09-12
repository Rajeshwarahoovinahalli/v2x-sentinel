
"""
fetch_firmware.py
==================
Downloads the latest firmware .bin from the cloud (GitHub) before a
FOTA update - replacing manual scp with a real, automated cloud-fetch
step, closing the "Cloud Server" layer from the original architecture.

Usage:
    python3 fetch_firmware.py                # downloads to firmware.bin
    python3 fetch_firmware.py --out other.bin # custom output filename

The downloaded file can then be passed straight into the existing
gateway_can_fota.c / gateway_pi_chunked.c programs via their
"Set .bin file path" menu option.
"""

import argparse
import hashlib
import os
import sys
import urllib.request

FIRMWARE_URL = "https://raw.githubusercontent.com/Rajeshwarahoovinahalli/v2x-sentinel-firmware/main/AppB.bin"

def fetch_firmware(url: str, out_path: str) -> bool:
    print(f"GATEWAY: Fetching firmware from cloud...")
    print(f"  URL: {url}")

    try:
        req = urllib.request.Request(url, headers={"User-Agent": "V2X-Sentinel-Gateway/1.0"})
        with urllib.request.urlopen(req, timeout=15) as response:
            if response.status != 200:
                print(f"GATEWAY: ERROR - server returned HTTP {response.status}")
                return False
            data = response.read()
    except Exception as e:
        print(f"GATEWAY: ERROR - download failed: {e}")
        return False

    if len(data) == 0:
        print("GATEWAY: ERROR - downloaded file is empty")
        return False

    if data[:15].lower().startswith(b"<!doctype html") or data[:5].lower().startswith(b"<html"):
        print("GATEWAY: ERROR - downloaded content looks like an HTML page, not a binary firmware file.")
        print("         Check the URL is correct and publicly accessible.")
        return False

    with open(out_path, "wb") as f:
        f.write(data)

    sha256 = hashlib.sha256(data).hexdigest()
    print(f"GATEWAY: Downloaded {len(data)} bytes -> {out_path}")
    print(f"GATEWAY_SECURITY: SHA-256 of downloaded file: {sha256}")
    return True

def main():
    parser = argparse.ArgumentParser(description="Fetch firmware from the cloud for FOTA.")
    parser.add_argument("--url", default=FIRMWARE_URL, help="Firmware URL to download")
    parser.add_argument("--out", default="firmware.bin", help="Local output filename")
    args = parser.parse_args()

    ok = fetch_firmware(args.url, args.out)
    if not ok:
        sys.exit(1)

    print(f"GATEWAY: Ready. Use '{args.out}' as the FOTA source file (gateway option 7).")

if __name__ == "__main__":
    main()
