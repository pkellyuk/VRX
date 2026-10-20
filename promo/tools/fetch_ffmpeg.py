"""Put a modern ffmpeg in promo/tools so the film can encode.

    python promo/tools/fetch_ffmpeg.py

Downloads the gyan.dev essentials build and unzips it next to this script. The
film prefers this over whatever is on PATH: some Python distributions ship an
ffmpeg from 2013, whose x264 is a decade behind and which does not understand
`-hide_banner`.

Already fetched is a no-op, so it is safe to run in a build script.
"""
from __future__ import annotations

import io
import sys
import urllib.request
import zipfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
URL = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip"


def existing() -> list[Path]:
    return sorted(TOOLS.glob("ffmpeg-*/bin/ffmpeg.exe")) + \
        sorted(TOOLS.glob("ffmpeg-*/bin/ffmpeg"))


def main() -> int:
    if existing():
        print(f"already present: {existing()[0]}")
        return 0

    print(f"downloading {URL}")
    with urllib.request.urlopen(URL, timeout=300) as response:
        payload = response.read()
    print(f"  {len(payload)/1e6:.1f} MB")

    with zipfile.ZipFile(io.BytesIO(payload)) as zf:
        names = [n for n in zf.namelist() if "/bin/" in n and n.endswith(
            ("ffmpeg.exe", "ffprobe.exe", "ffmpeg", "ffprobe"))]
        if not names:
            print("the archive does not look like an ffmpeg build", file=sys.stderr)
            return 1
        for name in names:
            zf.extract(name, TOOLS)
            print(f"  extracted {name}")

    found = existing()
    if not found:
        print("extracted, but no ffmpeg binary was found", file=sys.stderr)
        return 1
    print(f"ready: {found[0]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
