"""Report which render toolchain pieces are available on this machine."""
import importlib
import shutil
import subprocess
import sys

print("python", sys.version.split()[0], sys.executable)
for name in ["numpy", "cv2", "PIL", "scipy"]:
    try:
        m = importlib.import_module(name)
        print(f"  {name:8s} OK  {getattr(m, '__version__', '?')}")
    except Exception as exc:
        print(f"  {name:8s} MISSING ({exc.__class__.__name__})")

for exe in ["ffmpeg", "ffprobe"]:
    path = shutil.which(exe)
    print(f"{exe}: {path}")
    if path:
        out = subprocess.run([exe, "-hide_banner", "-version"], capture_output=True, text=True)
        line = (out.stdout or out.stderr).splitlines()[0] if (out.stdout or out.stderr) else "?"
        print("   ", line)
        if exe == "ffmpeg":
            enc = subprocess.run([exe, "-hide_banner", "-encoders"], capture_output=True, text=True)
            for key in ["libx264", "libx265", "aac", "libmp3lame", "h264_nvenc"]:
                hit = [l.strip() for l in enc.stdout.splitlines() if key in l]
                print(f"    encoder {key:12s} {'YES' if hit else 'no':4s} {hit[0][:70] if hit else ''}")
