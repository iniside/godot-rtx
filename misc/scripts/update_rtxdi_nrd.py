import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Optional, Tuple


RTXDI_REPOSITORY = "https://github.com/NVIDIA-RTX/RTXDI-Library.git"
RTXDI_COMMIT = "f12037fa8e97ebc08e9e3edfd2de528ed1772a4b"
NRD_REPOSITORY = "https://github.com/NVIDIA-RTX/NRD.git"
NRD_COMMIT = "2cd553031abec76bc4e109f20ef40a23ef0645c7"
SHADERMAKE_REPOSITORY = "https://github.com/NVIDIA-RTX/ShaderMake.git"
SHADERMAKE_COMMIT = "18f5a344e7ca8fa65daaf079d07bc8ce38453e05"
MATHLIB_REPOSITORY = "https://github.com/NVIDIA-RTX/MathLib.git"
MATHLIB_COMMIT = "974e1387ba936740c7cdc494792d2641bc127e86"


def run(*args: str, cwd: Optional[Path] = None) -> None:
    subprocess.run(args, cwd=cwd, check=True)


def clone(repository: str, commit: str, destination: Path) -> None:
    run("git", "clone", "--filter=blob:none", "--no-checkout", repository, str(destination))
    run("git", "checkout", "--detach", commit, cwd=destination)
    resolved_commit = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=destination, text=True
    ).strip()
    if resolved_commit != commit:
        raise RuntimeError(f"Expected {commit}, resolved {resolved_commit}")


def apply_patches(source: Path, patches: Path) -> None:
    if not patches.is_dir():
        return
    for patch in sorted(patches.glob("*.patch")):
        run("git", "apply", "--check", str(patch), cwd=source)
        run("git", "apply", str(patch), cwd=source)


def copy_tree(source: Path, destination: Path) -> None:
    shutil.copytree(source, destination)


def copy_file(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def generate_nrd_spirv(
    nrd_source: Path,
    shadermake_source: Path,
    mathlib_source: Path,
    build_directory: Path,
    jobs: int,
) -> None:
    run(
        "cmake",
        "-S",
        str(nrd_source),
        "-B",
        str(build_directory),
        "-DNRD_STATIC_LIBRARY=ON",
        "-DNRD_EMBEDS_SPIRV_SHADERS=ON",
        "-DNRD_EMBEDS_DXIL_SHADERS=OFF",
        "-DNRD_EMBEDS_DXBC_SHADERS=OFF",
        "-DSHADERMAKE_FIND_DXC_VK=OFF",
        f"-DFETCHCONTENT_SOURCE_DIR_SHADERMAKE={shadermake_source}",
        f"-DFETCHCONTENT_SOURCE_DIR_MATHLIB={mathlib_source}",
    )
    run(
        "cmake",
        "--build",
        str(build_directory),
        "--config",
        "Release",
        "--target",
        "NRDShaders",
        "--parallel",
        str(jobs),
    )


def stage_import(work_directory: Path, patches_root: Path, jobs: int) -> Tuple[Path, Path]:
    sources = work_directory / "sources"
    staged = work_directory / "staged"
    sources.mkdir()
    staged.mkdir()

    rtxdi_source = sources / "rtxdi"
    nrd_source = sources / "nrd"
    shadermake_source = sources / "shadermake"
    mathlib_source = sources / "mathlib"

    clone(RTXDI_REPOSITORY, RTXDI_COMMIT, rtxdi_source)
    clone(NRD_REPOSITORY, NRD_COMMIT, nrd_source)
    clone(SHADERMAKE_REPOSITORY, SHADERMAKE_COMMIT, shadermake_source)
    clone(MATHLIB_REPOSITORY, MATHLIB_COMMIT, mathlib_source)

    apply_patches(rtxdi_source, patches_root / "rtxdi")
    apply_patches(nrd_source, patches_root / "nrd")

    generate_nrd_spirv(
        nrd_source,
        shadermake_source,
        mathlib_source,
        work_directory / "nrd-build",
        jobs,
    )

    staged_rtxdi = staged / "rtxdi"
    copy_tree(rtxdi_source / "Include", staged_rtxdi / "Include")
    copy_tree(rtxdi_source / "Source", staged_rtxdi / "Source")
    copy_file(rtxdi_source / "LICENSE.txt", staged_rtxdi / "LICENSE.txt")
    if (patches_root / "rtxdi").is_dir():
        copy_tree(patches_root / "rtxdi", staged_rtxdi / "patches")

    staged_nrd = staged / "nrd"
    copy_tree(nrd_source / "Include", staged_nrd / "Include")
    copy_tree(nrd_source / "Source", staged_nrd / "Source")
    copy_tree(nrd_source / "Shaders", staged_nrd / "Shaders")
    copy_tree(nrd_source / "_Shaders", staged_nrd / "_Shaders")
    copy_file(nrd_source / "Resources" / "Version.h", staged_nrd / "Resources" / "Version.h")
    copy_file(nrd_source / "LICENSE.txt", staged_nrd / "LICENSE.txt")
    if (patches_root / "nrd").is_dir():
        copy_tree(patches_root / "nrd", staged_nrd / "patches")

    staged_mathlib = staged_nrd / "Dependencies" / "MathLib"
    copy_tree(mathlib_source / "Guts", staged_mathlib / "Guts")
    copy_file(mathlib_source / "ml.h", staged_mathlib / "ml.h")
    copy_file(mathlib_source / "ml.hlsli", staged_mathlib / "ml.hlsli")
    copy_file(mathlib_source / "LICENSE.txt", staged_mathlib / "LICENSE.txt")

    staged_shadermake = staged_nrd / "Dependencies" / "ShaderMake"
    copy_file(
        shadermake_source / "ShaderMake" / "ShaderBlob.cpp",
        staged_shadermake / "ShaderMake" / "ShaderBlob.cpp",
    )
    copy_file(
        shadermake_source / "ShaderMake" / "ShaderBlob.h",
        staged_shadermake / "ShaderMake" / "ShaderBlob.h",
    )
    copy_file(shadermake_source / "LICENSE.txt", staged_shadermake / "LICENSE.txt")
    return staged_rtxdi, staged_nrd


def replace_import(staged: Path, destination: Path) -> None:
    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(staged, destination)


def main() -> None:
    parser = argparse.ArgumentParser(description="Import pinned RTXDI and NRD sources.")
    parser.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 1))
    args = parser.parse_args()

    repository_root = Path(__file__).resolve().parents[2]
    thirdparty_root = repository_root / "thirdparty"
    if not (repository_root / "SConstruct").is_file() or not thirdparty_root.is_dir():
        raise RuntimeError("Run this script from a Godot source checkout")

    with tempfile.TemporaryDirectory(prefix="godot-rtxdi-nrd-") as temporary_directory:
        patches_root = Path(temporary_directory) / "patches"
        for dependency in ("rtxdi", "nrd"):
            source_patches = thirdparty_root / dependency / "patches"
            if source_patches.is_dir():
                copy_tree(source_patches, patches_root / dependency)
        staged_rtxdi, staged_nrd = stage_import(Path(temporary_directory), patches_root, args.jobs)
        replace_import(staged_rtxdi, thirdparty_root / "rtxdi")
        replace_import(staged_nrd, thirdparty_root / "nrd")


if __name__ == "__main__":
    main()
