"""Verify the isolated Ubuntu guest without model credentials."""
from pathlib import Path
import subprocess
import sys

desktop = Path(__file__).resolve().parent
worker = desktop / "VRX.VrexHost/bin/Release/net10.0-windows/VRX.VrexHost.exe"
sys.exit(subprocess.run([str(worker), "--ubuntu-self-test"], check=False).returncode)
