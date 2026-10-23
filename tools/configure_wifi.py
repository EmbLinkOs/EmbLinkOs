#!/usr/bin/env python3
"""Write a local WPA2 profile for the next OS image, without echoing its key."""
import argparse
import getpass
import hashlib
import os
from pathlib import Path
import tempfile


def encode_profile(ssid, password):
    if not 1 <= len(ssid) <= 32:
        raise ValueError("SSID must contain 1 to 32 bytes")
    if len(password) == 64:
        try:
            pmk = bytes.fromhex(password)
        except ValueError:
            raise ValueError("A 64-character PSK must be hexadecimal") from None
        if len(pmk) != 32:
            raise ValueError("A 64-character PSK must be hexadecimal")
    else:
        if not 8 <= len(password) <= 63 or any(not 32 <= ord(c) <= 126 for c in password):
            raise ValueError("WPA2 passphrase must contain 8 to 63 printable ASCII characters")
        pmk = hashlib.pbkdf2_hmac("sha1", password.encode("ascii"), ssid, 4096, 32)
    return f"ssid_hex={ssid.hex()}\npsk_hex={pmk.hex()}\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ssid", help="SSID (UTF-8); prompted when omitted")
    parser.add_argument("--ssid-hex", help="SSID as hexadecimal bytes instead of UTF-8")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "system/config/wifi.conf")
    args = parser.parse_args()
    if args.ssid is not None and args.ssid_hex is not None:
        parser.error("use --ssid or --ssid-hex")
    try:
        ssid = bytes.fromhex(args.ssid_hex) if args.ssid_hex is not None else (args.ssid if args.ssid is not None else input("SSID: ")).encode("utf-8")
        text = encode_profile(ssid, getpass.getpass("WPA2 passphrase or 64-digit PSK: "))
    except ValueError as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=".wifi.conf.", dir=args.output.parent)
    try:
        with os.fdopen(fd, "w", encoding="ascii") as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, args.output)
    finally:
        if os.path.exists(name):
            os.unlink(name)
    print(f"Profile written to {args.output}. Rebuild the boot image; the key is local and ignored by Git.")


if __name__ == "__main__":
    main()
