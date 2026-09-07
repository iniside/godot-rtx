import argparse
import hashlib
import io
import json
import urllib.request
import zipfile
from pathlib import Path


VERSION = "2026.13.1"
ARCHIVE = f"slang-{VERSION}-windows-x86_64.zip"
URL = f"https://github.com/shader-slang/slang/releases/download/v{VERSION}/{ARCHIVE}"
SHA256 = "fa1c9bcab2cdcd3626f7a1e250dd35d606c1b84745b64627f1dd63fca3746a70"
HEADERS = ("slang.h", "slang-deprecated.h", "slang-com-ptr.h", "slang-com-helper.h", "slang-image-format-defs.h", "slang-tag-version.h")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--archive", type=Path)
    args = parser.parse_args()
    data = args.archive.read_bytes() if args.archive else urllib.request.urlopen(URL).read()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise RuntimeError("Slang archive SHA-256 mismatch")
    destination = Path(__file__).resolve().parents[2] / "thirdparty" / "slang"
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        files = {f"include/{name}": archive.read(f"include/{name}").replace(b"\r\n", b"\n") for name in HEADERS}
        files["LICENSE"] = archive.read("LICENSE").replace(b"\r\n", b"\n")
        files[f"slang-compiler.{VERSION}.x86_64.dll"] = archive.read("bin/slang-compiler.dll")
    manifest = {"version": VERSION, "url": URL, "sha256": SHA256, "files": {name: hashlib.sha256(content).hexdigest() for name, content in files.items()}}
    for name, content in files.items():
        output = destination / name
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(content)
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Imported verified Slang {VERSION} to {destination}")


if __name__ == "__main__":
    main()
