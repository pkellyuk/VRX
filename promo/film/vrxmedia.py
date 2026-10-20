"""Shared media helpers for the VRX promo build.

Locates a modern ffmpeg, pipes raw frames into libx264, writes WAV scores and
muxes the two together. Everything the film scripts need for output lives here.
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np

PROMO = Path(__file__).resolve().parents[1]
TOOLS = PROMO / "tools"
BUILD = PROMO / "build"

# The bundled essentials build is preferred: the ffmpeg on PATH is a 2013 build
# with a much older libx264.
_BUNDLED = sorted((TOOLS).glob("ffmpeg-*-essentials_build/bin/ffmpeg.exe"))


def _find(name: str) -> str:
    exe = name + (".exe" if sys.platform == "win32" else "")
    for cand in sorted(TOOLS.glob(f"ffmpeg-*/bin/{exe}")):
        return str(cand)
    found = shutil.which(name)
    if found:
        return found
    raise FileNotFoundError(f"{name} not found; run tools/fetch_ffmpeg.py")


FFMPEG = _find("ffmpeg")
FFPROBE = _find("ffprobe")


def ensure_build_dir() -> Path:
    BUILD.mkdir(parents=True, exist_ok=True)
    return BUILD


def run(args: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(args, capture_output=True, text=True, **kw)


def probe(path: str | Path) -> dict:
    """Return {streams: [...], format: {...}} for a media file."""
    out = run([
        FFPROBE, "-v", "error", "-print_format", "json",
        "-show_format", "-show_streams", str(path),
    ])
    if out.returncode != 0:
        raise RuntimeError(f"ffprobe failed on {path}: {out.stderr}")
    return json.loads(out.stdout)


def video_summary(path: str | Path) -> dict:
    info = probe(path)
    v = next(s for s in info["streams"] if s["codec_type"] == "video")
    a = next((s for s in info["streams"] if s["codec_type"] == "audio"), None)
    num, den = (v.get("avg_frame_rate") or "0/1").split("/")
    fps = float(num) / float(den) if float(den) else 0.0
    return {
        "width": int(v["width"]),
        "height": int(v["height"]),
        "fps": round(fps, 3),
        "vcodec": v["codec_name"],
        "pix_fmt": v.get("pix_fmt"),
        "nb_frames": int(v.get("nb_frames") or 0),
        "duration": float(info["format"]["duration"]),
        "acodec": a["codec_name"] if a else None,
        "achannels": int(a["channels"]) if a else 0,
        "asample_rate": int(a["sample_rate"]) if a else 0,
    }


class FrameWriter:
    """Pipe raw frames straight into libx264.

    Frames are **RGB**, in the order the whole film works in (vrx_film.py,
    vrx_dibr.py and vrx_draw.py all produce RGB), and the pipe is declared as
    rgb24 to match. Getting this wrong swaps red and blue through the entire
    output without any error being raised, so tools/verify_promo.py checks the
    colour order of the finished file against a fresh render.

    Usage:
        with FrameWriter(out, 1920, 1080, 30) as w:
            for frame in frames:
                w.write(frame)
    """

    def __init__(self, out: str | Path, width: int, height: int, fps: int,
                 crf: int = 16, preset: str = "slow", tune: str | None = None,
                 pix_fmt: str = "yuv420p"):
        self.out = str(out)
        self.width, self.height, self.fps = width, height, fps
        self.crf, self.preset, self.tune, self.pix_fmt = crf, preset, tune, pix_fmt
        self.proc: subprocess.Popen | None = None
        self.frames = 0

    def _args(self) -> list[str]:
        args = [
            FFMPEG, "-y", "-hide_banner", "-loglevel", "error",
            "-f", "rawvideo", "-pix_fmt", "rgb24",
            "-s", f"{self.width}x{self.height}", "-r", str(self.fps),
            "-i", "pipe:0",
            "-an",
            "-c:v", "libx264", "-preset", self.preset, "-crf", str(self.crf),
            "-pix_fmt", self.pix_fmt,
            # Widely compatible H.264: 4.1 is enough for 1080p30 and for 1080x1920.
            "-profile:v", "high", "-level", "4.1",
            "-movflags", "+faststart",
        ]
        if self.tune:
            args += ["-tune", self.tune]
        args += [self.out]
        return args

    def __enter__(self) -> "FrameWriter":
        self.proc = subprocess.Popen(self._args(), stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return self

    def write(self, frame: np.ndarray) -> None:
        assert self.proc and self.proc.stdin
        h, w = frame.shape[:2]
        if (w, h) != (self.width, self.height):
            raise ValueError(f"frame is {w}x{h}, expected {self.width}x{self.height}")
        if frame.dtype != np.uint8:
            raise ValueError(f"frame dtype {frame.dtype}, expected uint8")
        self.proc.stdin.write(np.ascontiguousarray(frame).tobytes())
        self.frames += 1

    def __exit__(self, exc_type, exc, tb) -> None:
        assert self.proc
        if self.proc.stdin:
            self.proc.stdin.close()
        err = self.proc.stderr.read().decode("utf-8", "replace") if self.proc.stderr else ""
        self.proc.wait()
        if exc_type is None and self.proc.returncode != 0:
            raise RuntimeError(f"ffmpeg encode failed:\n{err}")


def write_wav(path: str | Path, audio: np.ndarray, rate: int) -> Path:
    """Write float audio in [-1, 1] as 16-bit PCM WAV. Accepts mono or stereo."""
    import wave

    path = Path(path)
    if audio.ndim == 1:
        audio = audio[:, None]
    peak = float(np.max(np.abs(audio))) if audio.size else 0.0
    if peak > 1.0:
        audio = audio / peak
    pcm = np.clip(audio * 32767.0, -32768, 32767).astype("<i2")
    with wave.open(str(path), "wb") as f:
        f.setnchannels(audio.shape[1])
        f.setsampwidth(2)
        f.setframerate(rate)
        f.writeframes(pcm.tobytes())
    return path


def verify_encoded(path: str | Path, width: int, height: int, frames: int,
                   fps: int, *, tolerance_frames: int = 2) -> list[str]:
    """Read back an encoded file and list anything wrong with it.

    An encoder that dies part way, or a file that is written over by a second
    process, leaves behind something that still has a plausible header: a short
    duration, a frame count that does not match, or a stream that will not
    decode. None of that raises an exception at encode time, so the only way to
    know is to read the file back. An empty list means it is good.
    """
    path = Path(path)
    problems: list[str] = []
    if not path.exists():
        return [f"{path.name} does not exist"]
    if path.stat().st_size == 0:
        return [f"{path.name} is empty"]

    info = probe(path)
    video = next((s for s in info["streams"] if s["codec_type"] == "video"), None)
    if video is None:
        return [f"{path.name} has no video stream"]
    if (int(video["width"]), int(video["height"])) != (width, height):
        problems.append(f"{path.name} is {video['width']}x{video['height']}, "
                        f"want {width}x{height}")
    pix = video.get("pix_fmt")
    if not pix or pix == "unknown":
        problems.append(f"{path.name} has no readable pixel format ({pix!r})")
    if not video.get("profile") or video.get("profile") == "unknown":
        problems.append(f"{path.name} has no readable H.264 profile")

    # The container's frame count, and the count ffmpeg actually decodes.
    counted = run([FFPROBE, "-v", "error", "-count_frames", "-select_streams", "v:0",
                   "-show_entries", "stream=nb_read_frames", "-of", "csv=p=0", str(path)])
    text = (counted.stdout or "").strip().splitlines()
    decoded = int(text[0]) if text and text[0].isdigit() else -1
    if decoded < 0:
        problems.append(f"{path.name}: could not count decoded frames")
    elif abs(decoded - frames) > tolerance_frames:
        problems.append(f"{path.name} decodes {decoded} frames, want {frames}")

    noise = run([FFMPEG, "-v", "error", "-i", str(path), "-f", "null", "-"])
    complaint = (noise.stderr or "").strip()
    if noise.returncode != 0 or complaint:
        problems.append(f"{path.name} decodes with errors: {complaint[:160]}")
    return problems


def encode_verified(out: str | Path, width: int, height: int, fps: int,
                    make_frames, frames: int, *, crf: int = 16,
                    preset: str = "slow", attempts: int = 2) -> Path:
    """Encode frames to `out`, reading the result back before trusting it.

    `make_frames` is called once per attempt and must return a fresh iterator, so
    a retry re-renders rather than consuming an exhausted generator. Writes to a
    temporary file and only replaces the target once the file has been verified,
    so a failed or interrupted encode can never leave a half-written deliverable
    in place of a good one.
    """
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    last: list[str] = []
    for attempt in range(1, attempts + 1):
        tmp = out.with_name(out.stem + ".encoding.mp4")
        with FrameWriter(tmp, width, height, fps, crf=crf, preset=preset) as w:
            for frame in make_frames():
                w.write(frame)
        last = verify_encoded(tmp, width, height, frames, fps)
        if not last:
            tmp.replace(out)
            return out
        print(f"    encode attempt {attempt} failed verification:")
        for p in last:
            print(f"      - {p}")
        if attempt == attempts:
            tmp.unlink(missing_ok=True)
    raise RuntimeError(f"{out.name} did not verify after {attempts} attempts: {last}")


def mux(video: str | Path, audio: str | Path, out: str | Path,
        audio_bitrate: str = "192k") -> None:
    """Combine a silent master with its score, copying the video stream.

    Muxes to a temporary file and replaces the target only on success, so the
    previous good deliverable survives a failed mux.
    """
    out = Path(out)
    tmp = out.with_name(out.stem + ".muxing.mp4")
    res = run([
        FFMPEG, "-y", "-hide_banner", "-loglevel", "error",
        "-i", str(video), "-i", str(audio),
        "-map", "0:v:0", "-map", "1:a:0",
        "-c:v", "copy", "-c:a", "aac", "-b:a", audio_bitrate,
        "-ar", "48000", "-ac", "2",
        "-shortest", "-movflags", "+faststart", str(tmp),
    ])
    if res.returncode != 0:
        tmp.unlink(missing_ok=True)
        raise RuntimeError(f"mux failed:\n{res.stderr}")
    tmp.replace(out)


if __name__ == "__main__":
    print("ffmpeg ", FFMPEG)
    print("ffprobe", FFPROBE)
    print(run([FFMPEG, "-version"]).stdout.splitlines()[0])
