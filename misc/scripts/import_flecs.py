#!/usr/bin/env python3

import hashlib
import json
from pathlib import Path
from urllib.request import urlopen


REVISION = "fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8"
UPSTREAM = "https://github.com/SanderMertens/flecs"
FILES = {"distr/flecs.h": "flecs.h", "distr/flecs.c": "flecs.c", "LICENSE": "LICENSE"}


def main():
    destination = Path(__file__).resolve().parents[2] / "thirdparty" / "flecs"
    payloads = {}
    for source, name in FILES.items():
        with urlopen(f"https://raw.githubusercontent.com/SanderMertens/flecs/{REVISION}/{source}") as response:
            payloads[name] = response.read()
    destination.mkdir(parents=True, exist_ok=True)
    for name, content in payloads.items():
        path = destination / name
        if path.exists() and path.read_bytes() != content:
            raise SystemExit(f"Refusing to overwrite modified vendor file: {path}")
    for name, content in payloads.items():
        (destination / name).write_bytes(content)
    provenance = {
        "upstream": UPSTREAM,
        "revision": REVISION,
        "version": "4.1.6",
        "recipe": "python misc/scripts/import_flecs.py",
        "files": {name: hashlib.sha256(content).hexdigest() for name, content in payloads.items()},
    }
    (destination / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
