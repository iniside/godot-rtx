#!/usr/bin/env python3

import hashlib
import json
from pathlib import Path
from urllib.request import urlopen

REVISION = "6ffccbdb1000253d8d513dd7a5ae9226e5023a5c"
UPSTREAM = "https://github.com/dougbinks/enkiTS"
FILES = {
    "src/TaskScheduler.h": "TaskScheduler.h",
    "src/TaskScheduler.cpp": "TaskScheduler.cpp",
    "src/LockLessMultiReadPipe.h": "LockLessMultiReadPipe.h",
    "License.txt": "License.txt",
}


def main():
    destination = Path(__file__).resolve().parents[2] / "thirdparty" / "enkits"
    payloads = {}
    for source, name in FILES.items():
        with urlopen(f"https://raw.githubusercontent.com/dougbinks/enkiTS/{REVISION}/{source}") as response:
            payloads[name] = response.read()
    provenance = {
        "upstream": UPSTREAM,
        "revision": REVISION,
        "version": "1.11",
        "recipe": "python misc/scripts/import_enkits.py",
        "files": {name: hashlib.sha256(content).hexdigest() for name, content in payloads.items()},
    }
    payloads["provenance.json"] = (json.dumps(provenance, indent=2) + "\n").encode("utf-8")
    destination.mkdir(parents=True, exist_ok=True)
    for name, content in payloads.items():
        path = destination / name
        if path.exists() and path.read_bytes() != content:
            raise SystemExit(f"Refusing to overwrite modified vendor file: {path}")
    for name, content in payloads.items():
        path = destination / name
        if not path.exists():
            path.write_bytes(content)


if __name__ == "__main__":
    main()
