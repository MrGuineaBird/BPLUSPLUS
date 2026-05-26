import os
import hashlib
import json
import re
import subprocess
import sys


ROOT = os.path.dirname(os.path.abspath(__file__))
DIST = os.path.join(ROOT, "dist")
BUILD = os.path.join(ROOT, "build")
COMPILER_SOURCE = os.path.join(ROOT, "b++.py")
UPDATE_REPO = "MrGuineaBird/BPLUSPLUS"
IS_WINDOWS = os.name == "nt"


def run(command):
    print(" ".join(command))
    completed = subprocess.run(command, cwd=ROOT, check=False)
    if completed.returncode != 0:
        raise SystemExit(completed.returncode)


def ensure_pyinstaller():
    completed = subprocess.run(
        [sys.executable, "-m", "PyInstaller", "--version"],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        print("PyInstaller is not installed for this Python.")
        print(f"Install it with: {sys.executable} -m pip install pyinstaller")
        raise SystemExit(1)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def current_version():
    with open(COMPILER_SOURCE, "r", encoding="utf-8") as handle:
        text = handle.read()
    match = re.search(r'^CURRENT_VERSION\s*=\s*"([^"]+)"', text, re.MULTILINE)
    if not match:
        raise SystemExit("Could not find CURRENT_VERSION in b++.py")
    return match.group(1)


def write_update_manifest(payload):
    version = current_version()
    asset_name = os.path.basename(payload)
    manifest = {
        "version": version,
        "bpp_url": f"https://github.com/{UPDATE_REPO}/releases/download/v{version}/{asset_name}",
        "sha256": sha256(payload),
    }
    path = os.path.join(DIST, "latest.example.json")
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")
    return path


def main():
    ensure_pyinstaller()
    os.makedirs(DIST, exist_ok=True)
    os.makedirs(BUILD, exist_ok=True)

    run(
        [
            sys.executable,
            "-m",
            "PyInstaller",
            "--noconfirm",
            "--clean",
            "--onefile",
            "--console",
            "--name",
            "bpp",
            "--distpath",
            DIST,
            "--workpath",
            os.path.join(BUILD, "bpp_cli"),
            "--specpath",
            BUILD,
            COMPILER_SOURCE,
        ]
    )

    payload = os.path.join(DIST, "bpp.exe" if IS_WINDOWS else "bpp")
    if not os.path.exists(payload):
        raise SystemExit("Expected compiler payload was not built: " + payload)
    manifest = write_update_manifest(payload)

    if IS_WINDOWS:
        run(
            [
                sys.executable,
                "-m",
                "PyInstaller",
                "--noconfirm",
                "--clean",
                "--onefile",
                "--windowed",
                "--name",
                "B++ Setup",
                "--add-binary",
                payload + os.pathsep + ".",
                "--distpath",
                DIST,
                "--workpath",
                os.path.join(BUILD, "bpp_setup"),
                "--specpath",
                BUILD,
                os.path.join(ROOT, "bpp_setup.py"),
            ]
        )

    print()
    print("Built:")
    print(payload)
    if IS_WINDOWS:
        print(os.path.join(DIST, "B++ Setup.exe"))
    print(manifest)


if __name__ == "__main__":
    main()
