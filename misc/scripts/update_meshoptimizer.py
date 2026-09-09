import argparse
import hashlib
import json
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path


REPOSITORY = "https://github.com/zeux/meshoptimizer.git"
COMMIT = "0870c3881655df9b7d22faa35c825393534416bc"
PREVIOUS_COMMIT = "b22872835dbabc56a6e4a366ea9917f62b7daf1a"
INPUTS = ("src", "LICENSE.md", "demo/clusterlod.h")
PREVIOUS_INPUTS = ("src", "LICENSE.md")
PREVIOUS_README = """## meshoptimizer

- Upstream: https://github.com/zeux/meshoptimizer
- Version: 1.1.1 (b22872835dbabc56a6e4a366ea9917f62b7daf1a, 2026)
- License: MIT

Files extracted from upstream repository:

- All files in `src/`
- `LICENSE.md`
"""
README = """## meshoptimizer

- Upstream: https://github.com/zeux/meshoptimizer
- Version: git (0870c3881655df9b7d22faa35c825393534416bc, 2026)
- License: MIT

Files extracted from upstream repository:

- All files in `src/`
- `LICENSE.md`
- `demo/clusterlod.h` as `clusterlod.h`

`import.json` records the pinned source and hashes of the upstream and imported
files. Run `python misc/scripts/update_meshoptimizer.py` to reproduce the import.
The optional `--source` accepts a local upstream Git repository; files are
always read from the pinned commits, never from its working tree.
"""


def run(*args, cwd=None):
    subprocess.run(args, cwd=cwd, check=True)


def manifest(root):
    return {
        path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def extract(source, commit, inputs, work):
    archive = work / f"{commit}.tar"
    run("git", "-c", "core.autocrlf=false", "archive", "--format=tar", f"--output={archive}", commit, *inputs, cwd=source)
    extracted = work / f"extracted-{commit}"
    extracted.mkdir()
    with tarfile.open(archive) as contents:
        contents.extractall(extracted, filter="data")
    return extracted


def stage_import(extracted, staged, include_clusterlod):
    staged.mkdir()
    for source in sorted((extracted / "src").iterdir()):
        if source.is_file():
            shutil.copyfile(source, staged / source.name)
    shutil.copyfile(extracted / "LICENSE.md", staged / "LICENSE.md")
    if include_clusterlod:
        shutil.copyfile(extracted / "demo/clusterlod.h", staged / "clusterlod.h")


def verify_commit(source, commit):
    resolved = subprocess.check_output(["git", "rev-parse", commit + "^{commit}"], cwd=source, text=True).strip()
    if resolved != commit:
        raise RuntimeError(f"Expected {commit}, resolved {resolved}")


def main():
    parser = argparse.ArgumentParser(description="Import the pinned meshoptimizer source and cluster LOD builder.")
    parser.add_argument("--source", type=Path, help="Existing upstream Git repository containing both pinned commits.")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    destination = root / "thirdparty/meshoptimizer"
    readme = root / "thirdparty/README.md"
    if not (root / "SConstruct").is_file():
        raise RuntimeError("Expected a Godot source checkout")

    with tempfile.TemporaryDirectory(prefix="godot-meshoptimizer-") as temporary:
        work = Path(temporary)
        source = args.source.resolve() if args.source else work / "upstream"
        if not args.source:
            run("git", "clone", "--filter=blob:none", "--no-checkout", REPOSITORY, str(source))
        verify_commit(source, PREVIOUS_COMMIT)
        verify_commit(source, COMMIT)

        previous_extracted = extract(source, PREVIOUS_COMMIT, PREVIOUS_INPUTS, work)
        previous_staged = work / "previous"
        stage_import(previous_extracted, previous_staged, False)

        extracted = extract(source, COMMIT, INPUTS, work)
        staged = work / "staged"
        stage_import(extracted, staged, True)
        imported_files = manifest(staged)
        provenance = {
            "repository": REPOSITORY,
            "commit": COMMIT,
            "version": "1.2-dev",
            "inputs": INPUTS,
            "layout": {
                "src/": ".",
                "LICENSE.md": "LICENSE.md",
                "demo/clusterlod.h": "clusterlod.h",
            },
            "normalization": "none; exact git archive bytes",
            "upstream_files": manifest(extracted),
            "imported_files": imported_files,
        }
        (staged / "import.json").write_bytes((json.dumps(provenance, indent=2) + "\n").encode("utf-8"))

        existing = manifest(destination)
        existing_without_manifest = {name: value for name, value in existing.items() if name != "import.json"}
        readme_text = readme.read_text(encoding="utf-8")
        if "import.json" in existing:
            previous = json.loads((destination / "import.json").read_text(encoding="utf-8"))
            if existing_without_manifest != previous["imported_files"]:
                raise RuntimeError("Existing meshoptimizer sources have local edits; preserve them before updating the import")
            documented_commit = previous["commit"]
            if documented_commit not in readme_text:
                raise RuntimeError(f"thirdparty/README.md does not document the installed meshoptimizer pin {documented_commit}")
        else:
            if existing_without_manifest != manifest(previous_staged):
                raise RuntimeError(f"Existing meshoptimizer sources do not match documented pin {PREVIOUS_COMMIT}")
            if PREVIOUS_README not in readme_text:
                raise RuntimeError(f"thirdparty/README.md does not document expected pin {PREVIOUS_COMMIT}")

        expected = manifest(staged)
        if existing == expected and README in readme_text:
            print(f"meshoptimizer {COMMIT}: import already matches ({len(imported_files)} source files).")
            return

        if PREVIOUS_README in readme_text:
            updated_readme = readme_text.replace(PREVIOUS_README, README, 1)
        elif README in readme_text:
            updated_readme = readme_text
        else:
            raise RuntimeError("Unexpected meshoptimizer provenance section in thirdparty/README.md")

        for name in existing.keys() - expected.keys():
            target = (destination / name).resolve()
            if not target.is_relative_to(destination.resolve()):
                raise RuntimeError(f"Unexpected import path: {target}")
            target.unlink()
        shutil.copytree(staged, destination, dirs_exist_ok=True)
        readme.write_text(updated_readme, encoding="utf-8", newline="\n")
        print(f"Imported meshoptimizer {COMMIT}: {len(imported_files)} source files.")


if __name__ == "__main__":
    main()
