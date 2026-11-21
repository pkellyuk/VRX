"""Package a QEMU runtime and its independently downloadable source assets."""

import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def git(repository, *arguments):
    return subprocess.check_output(["git", "-C", repository, *arguments])


def export(repository, output, prefix, commit="HEAD"):
    git(repository, "archive", "--format=tar.gz", f"--prefix={prefix}/",
        f"--output={output}", commit)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("runtime", "built-runtime", "qemu-checkout", "inputs", "output"):
        parser.add_argument(f"--{argument}", type=Path, required=True)
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    checkout = args.qemu_checkout.resolve()
    build = args.built_runtime.resolve()
    inputs = args.inputs.resolve()
    output = args.output.resolve()
    metadata = json.loads((runtime / "personabot-build.json").read_text(encoding="utf-8-sig"))
    commit = metadata["SourceCommit"]
    patch = build / "windows-egl-readback.patch"
    if git(checkout, "rev-parse", "HEAD").decode().strip() != commit:
        raise ValueError("The checkout revision does not match the runtime.")
    if sha256(patch) != metadata["RuntimePatchSha256"].lower():
        raise ValueError("The EGL patch does not match the runtime.")
    if git(checkout, "diff", "HEAD", "--", ".", ":(exclude)meson.build").strip() != patch.read_bytes().replace(b"\r\n", b"\n").strip():
        raise ValueError("The checkout's runtime changes differ from the build patch.")
    meson_patch = git(checkout, "diff", "HEAD", "--", "meson.build")
    hook = b"meson.add_postconf_script(find_program('scripts/symlink-install-tree.py'))"
    expected_meson = git(checkout, "show", "HEAD:meson.build").replace(
        hook, b"if host_machine.system() != 'windows'\n  " + hook + b"\nendif")
    if (checkout / "meson.build").read_bytes().replace(b"\r\n", b"\n").strip() != expected_meson.strip():
        raise ValueError("The checkout's Meson change does not match the build recipe.")

    binary_hashes = {}
    for file in sorted(runtime.rglob("*")):
        if file.is_file():
            name = file.relative_to(runtime).as_posix()
            digest = sha256(file)
            if digest != sha256(build / name):
                raise ValueError(f"The deployed runtime differs from the original build: {name}")
            binary_hashes[name] = digest

    packages = json.loads((inputs / "msys2-sources.json").read_text(encoding="utf-8-sig"))
    mapped_dlls = {name for package in packages for name in package["files"]}
    if mapped_dlls != {path.name for path in runtime.glob("*.dll")}:
        raise ValueError("The MSYS2 inventory does not match the runtime DLLs.")
    library_sources = [inputs / "msys2" / package["archive"] for package in packages]
    angle, = (path for path in library_sources if path.name.startswith("mingw-w64-angleproject-"))
    firmware = inputs / "firmware"
    firmware_files = {path.name for path in firmware.glob("*.tar.gz")}
    rom_archives = {
        f"{path.split('/')[-1]}-{revision}.tar.gz"
        for _, _, revision, path in (
            line.split() for line in git(checkout, "ls-tree", "-r", "HEAD", "roms").decode().splitlines()
            if line.startswith("160000 ")
        )
    }
    edk2 = json.loads((firmware / "edk2-sources.json").read_text(encoding="utf-8-sig"))
    required_firmware = rom_archives | {f"edk2-{item['name']}-{item['sha']}.tar.gz" for item in edk2}
    required_firmware |= {"U-Boot-v2021.07.tar.gz", "SoftFloat-b64af41c3276f97f0e181920400ee056b9c88037.tar.gz"}
    if missing := required_firmware - firmware_files:
        raise ValueError(f"Missing firmware source archives: {sorted(missing)}")

    output.mkdir(parents=True, exist_ok=False)
    runtime_zip = output / "vrx-qemu-win-x64.zip"
    source_zip = output / "vrx-qemu-source.zip"
    notices = {
        "QEMU-COPYING.txt": checkout / "COPYING",
        "QEMU-LICENSE.txt": checkout / "LICENSE",
        "QEMU-firmware.txt": checkout / "pc-bios" / "README",
    }
    with ZipFile(runtime_zip, "w", ZIP_DEFLATED, compresslevel=3) as archive:
        for name in binary_hashes:
            archive.write(runtime / name, name)
        for name, file in notices.items():
            archive.write(file, name)
    shutil.copyfile(angle, output / angle.name)
    with tempfile.TemporaryDirectory(dir=output) as temporary, ZipFile(source_zip, "w", ZIP_STORED) as archive:
        scratch = Path(temporary)
        source = scratch / "qemu.tar.gz"
        export(checkout, source, f"qemu-{commit}", commit)
        archive.write(source, source.name)
        for name in ("dtc", "keycodemapdb"):
            repository = checkout / "subprojects" / name
            revision = git(repository, "rev-parse", "HEAD").decode().strip()
            wrap = (checkout / "subprojects" / f"{name}.wrap").read_text(encoding="utf-8")
            if f"revision = {revision}" not in wrap:
                raise ValueError(f"{name} does not match the QEMU build dependency pin.")
            source = scratch / f"{name}-{revision}.tar.gz"
            export(repository, source, name, revision)
            archive.write(source, f"subprojects/{source.name}")
        archive.write(patch, patch.name)
        archive.writestr("windows-meson.patch", meson_patch)
        archive.write(build / "Build-SharedGpuQemu.ps1", "Build-SharedGpuQemu.ps1")
        archive.write(Path(__file__).with_name("QEMU-SOURCE-README.txt"), "README.txt")
        archive.write(runtime / "personabot-build.json", "build.json")
        archive.writestr("runtime-files.json", json.dumps(binary_hashes, indent=2))
        archive.writestr("msys2-sources.json", json.dumps(packages, indent=2))
        archive.writestr("edk2-sources.json", json.dumps(edk2, indent=2))
        for file in library_sources:
            if file != angle:
                archive.write(file, f"msys2/{file.name}")
        for name in sorted(required_firmware):
            archive.write(firmware / name, f"firmware/{name}")
        for name, file in notices.items():
            archive.write(file, f"licenses/{name}")
        for file in sorted((runtime / "share" / "licenses").rglob("*")):
            if file.is_file():
                archive.write(file, f"licenses/msys2/{file.relative_to(runtime / 'share' / 'licenses').as_posix()}")
    for file in (runtime_zip, source_zip):
        with ZipFile(file) as archive:
            if invalid := archive.testzip():
                raise ValueError(f"Invalid archive member in {file.name}: {invalid}")
    (output / "SHA256SUMS.txt").write_text(
        "".join(f"{sha256(file)}  {file.name}\n" for file in sorted(output.iterdir())),
        encoding="ascii",
    )


if __name__ == "__main__":
    main()
