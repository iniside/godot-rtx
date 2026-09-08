import argparse
import hashlib
import json
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path


REPOSITORY = "https://github.com/NVIDIAGameWorks/RTXGI-DDGI.git"
COMMIT = "f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6"
INPUTS = (
    "License.txt",
    "rtxgi-sdk/shaders",
    "rtxgi-sdk/include/rtxgi/Defines.h",
    "rtxgi-sdk/include/rtxgi/ddgi/DDGIRootConstants.h",
    "rtxgi-sdk/include/rtxgi/ddgi/DDGIVolumeDescGPU.h",
)


def run(*args, cwd=None):
    subprocess.run(args, cwd=cwd, check=True)


def manifest(root):
    return {
        path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def main():
    parser = argparse.ArgumentParser(description="Import the pinned RTXGI-DDGI shader and GPU header closure.")
    parser.add_argument("--source", type=Path, help="Existing upstream Git repository containing the pinned commit.")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    destination = root / "thirdparty/rtxgi_ddgi"
    patches = root / "misc/scripts/patches/rtxgi_ddgi"
    if not (root / "SConstruct").is_file():
        raise RuntimeError("Expected a Godot source checkout")

    with tempfile.TemporaryDirectory(prefix="godot-rtxgi-ddgi-") as temporary:
        work = Path(temporary)
        source = args.source.resolve() if args.source else work / "upstream"
        if not args.source:
            run("git", "clone", "--filter=blob:none", "--no-checkout", REPOSITORY, str(source))
        resolved = subprocess.check_output(["git", "rev-parse", COMMIT + "^{commit}"], cwd=source, text=True).strip()
        if resolved != COMMIT:
            raise RuntimeError(f"Expected {COMMIT}, resolved {resolved}")
        archive = work / "source.tar"
        run("git", "archive", "--format=tar", f"--output={archive}", COMMIT, *INPUTS, cwd=source)
        staged = work / "staged"
        staged.mkdir()
        with tarfile.open(archive) as contents:
            contents.extractall(staged, filter="data")
        upstream_files = manifest(staged)
        patch_files = sorted(patches.glob("*.patch"))
        for patch in patch_files:
            run("git", "apply", "--check", str(patch), cwd=staged)
            run("git", "apply", str(patch), cwd=staged)
        for path in staged.rglob("*"):
            if path.is_file():
                lines = path.read_bytes().replace(b"\r\n", b"\n").split(b"\n")
                path.write_bytes(b"\n".join(line.rstrip(b" \t") for line in lines))
        imported_files = manifest(staged)
        provenance = {
            "repository": REPOSITORY,
            "commit": COMMIT,
            "version": "1.3.6",
            "inputs": INPUTS,
            "normalization": "LF line endings and no trailing spaces/tabs",
            "patches": {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in patch_files},
            "upstream_files": upstream_files,
            "imported_files": imported_files,
        }
        (staged / "import.json").write_bytes((json.dumps(provenance, indent=2) + "\n").encode("utf-8"))
        if destination.exists():
            existing = manifest(destination)
            expected = manifest(staged)
            if existing == expected:
                print(f"RTXGI-DDGI {COMMIT}: import already matches ({len(imported_files)} source files).")
                return
            previous = json.loads((destination / "import.json").read_text(encoding="utf-8"))
            if {name: value for name, value in existing.items() if name != "import.json"} != previous["imported_files"]:
                raise RuntimeError("Existing RTXGI-DDGI sources have local edits; preserve them before updating the import")
            for name in existing.keys() - expected.keys():
                target = (destination / name).resolve()
                if not target.is_relative_to(destination.resolve()):
                    raise RuntimeError(f"Unexpected import path: {target}")
                target.unlink()
            shutil.copytree(staged, destination, dirs_exist_ok=True)
        else:
            shutil.copytree(staged, destination)
        print(f"Imported RTXGI-DDGI {COMMIT}: {len(imported_files)} source files, {len(patch_files)} patches.")


if __name__ == "__main__":
    main()
