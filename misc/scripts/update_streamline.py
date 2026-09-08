#!/usr/bin/env python3
"""Import the pinned official production Streamline/DLSS Windows payload."""

import argparse
import hashlib
import io
import json
from pathlib import Path
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SL_URL = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.12.0/streamline-sdk-v2.12.0.zip"
SL_SHA256 = "f5c0a3d870707dddc3570fb4bcd3655cf48a8a68c3a9d342910cfa21b77dcf48"
DLSS_BASE = "https://raw.githubusercontent.com/NVIDIA/DLSS/a291cc7d2cc642a51566f3dfd5376f635cd1b284/"


def checked(data, expected, name):
    if hashlib.sha256(data).hexdigest() != expected:
        raise RuntimeError("SHA256 mismatch: " + name)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, help="Use an already downloaded official SL archive")
    args = parser.parse_args()
    archive = args.archive.read_bytes() if args.archive else urllib.request.urlopen(SL_URL).read()
    sdk = zipfile.ZipFile(io.BytesIO(checked(archive, SL_SHA256, SL_URL)))
    vendor = ROOT / "thirdparty/streamline"
    manifest = json.loads((vendor / "runtime_manifest.json").read_text())
    payload = {}
    for name, digest in manifest.items():
        if name in ("nvngx_dlss.dll", "nvngx_dlssd.dll", "nvngx_dlssg.dll"):
            data = urllib.request.urlopen(DLSS_BASE + "lib/Windows_x86_64/rel/" + name).read()
        elif name == "dlss.LICENSE.txt":
            data = urllib.request.urlopen(DLSS_BASE + "LICENSE.txt").read()
        else:
            data = sdk.read("license.txt" if name == "streamline.LICENSE.txt" else "bin/x64/" + name)
        payload[name] = checked(data, digest, name)
    # Validate the complete payload before replacing any installed runtime files.
    for name in sdk.namelist():
        if name.startswith("include/") and name.endswith(".h"):
            destination = vendor / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(sdk.read(name))
    for directory in (vendor / "runtime", ROOT / "bin"):
        directory.mkdir(parents=True, exist_ok=True)
        for name, data in payload.items():
            (directory / name).write_bytes(data)
        (directory / "streamline.runtime_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("Imported verified Streamline 2.12.0 + DLSS 310.7.0 production payload. Effective GPU model remains unverified.")


if __name__ == "__main__":
    main()
