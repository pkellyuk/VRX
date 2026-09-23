#!/usr/bin/env python3
"""Early Linux session controller for the OpenXR diagnostic engine."""
import json
import os
from pathlib import Path
import sys
import tempfile

from PyQt6.QtCore import QProcess, QProcessEnvironment, QTimer
from PyQt6.QtWidgets import (
    QApplication, QCheckBox, QComboBox, QFormLayout, QHBoxLayout,
    QLabel, QLineEdit, QMainWindow, QMessageBox, QPushButton, QDoubleSpinBox,
    QPlainTextEdit, QVBoxLayout, QWidget,
)

ROOT = Path(__file__).resolve().parent.parent
ENGINE = Path(os.environ["VRX_LINUX_ENGINE"]).expanduser().resolve() if "VRX_LINUX_ENGINE" in os.environ else (
    ROOT / "build/linux-release/vrx-xr-synthetic"
    if (ROOT / "build/linux-release/vrx-xr-synthetic").is_file()
    else ROOT / "build/linux/vrx-xr-synthetic"
)
MODEL = ROOT / "bench/models/zipdepth_faithful_fp16_672x384.onnx"
CONFIG = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "vrx"
PROFILES = CONFIG / "linux-profiles.json"
DEFAULTS = {"cuda": True, "width": 2.0, "distance": 2.0,
            "height": 0.0, "horizontal": 0.0, "strength": 1.0}
RANGES = {"width": (0.5, 10.0), "distance": (0.5, 8.0),
          "height": (-2.0, 2.0), "horizontal": (-3.0, 3.0),
          "strength": (0.0, 2.0)}


def normalize_profile(value):
    if not isinstance(value, dict) or not isinstance(value.get("cuda"), bool):
        raise ValueError("profile must contain a CUDA choice")
    profile = {**DEFAULTS, **value}
    for key, (low, high) in RANGES.items():
        number = profile[key]
        if isinstance(number, bool) or not isinstance(number, (int, float)) or not low <= number <= high:
            raise ValueError(f"{key} must be between {low} and {high}")
        profile[key] = float(number)
    return {key: profile[key] for key in DEFAULTS}


def load_profiles():
    if not PROFILES.exists():
        return {"Default": DEFAULTS.copy()}
    try:
        document = json.loads(PROFILES.read_text(encoding="utf-8"))
        if document.get("version") != 1 or not isinstance(document.get("profiles"), dict):
            raise ValueError("unsupported profile format")
        profiles = {str(name): normalize_profile(value)
                    for name, value in document["profiles"].items()}
        return profiles or {"Default": DEFAULTS.copy()}
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise RuntimeError(f"Cannot load Linux profiles: {error}") from error


def save_profiles(profiles):
    CONFIG.mkdir(parents=True, exist_ok=True)
    temporary = PROFILES.with_suffix(".json.tmp")
    temporary.write_text(json.dumps({"version": 1, "profiles": profiles}, indent=2) + "\n",
                         encoding="utf-8")
    os.replace(temporary, PROFILES)


class Controller(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("VRX Linux")
        self.resize(680, 480)
        self.process = QProcess(self)
        self.process.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        self.process.readyReadStandardOutput.connect(self.read_output)
        self.process.finished.connect(self.finished)
        self.process.errorOccurred.connect(self.process_error)
        self.kill_timer = QTimer(self)
        self.kill_timer.setSingleShot(True)
        self.kill_timer.timeout.connect(self.force_stop)
        self.profiles = load_profiles()
        self.session_directory = tempfile.TemporaryDirectory(prefix="vrx-linux-")
        self.runtime_settings = Path(self.session_directory.name) / "settings"
        self.loading_profile = False
        self.settings_timer = QTimer(self)
        self.settings_timer.setSingleShot(True)
        self.settings_timer.timeout.connect(self.apply_runtime_settings)

        central = QWidget(self)
        layout = QVBoxLayout(central)
        form = QFormLayout()
        self.profile_name = QLineEdit("Default")
        self.profile_choices = QComboBox()
        self.profile_choices.addItems(sorted(self.profiles))
        self.profile_choices.currentTextChanged.connect(self.select_profile)
        form.addRow("Saved profile", self.profile_choices)
        form.addRow("Profile name", self.profile_name)
        self.cuda = QCheckBox("Use ZipDepth on CUDA")
        form.addRow("Depth", self.cuda)
        self.controls = {}
        for key, label, suffix in (
            ("width", "Screen width", " m"),
            ("distance", "Screen distance", " m"),
            ("height", "Screen height", " m"),
            ("horizontal", "Screen horizontal", " m"),
            ("strength", "Stereo strength", "×"),
        ):
            control = QDoubleSpinBox()
            control.setRange(*RANGES[key])
            control.setSingleStep(0.1)
            control.setDecimals(2)
            control.setSuffix(suffix)
            control.valueChanged.connect(self.settings_changed)
            form.addRow(label, control)
            self.controls[key] = control
        layout.addLayout(form)
        buttons = QHBoxLayout()
        self.save_button = QPushButton("Save profile")
        self.save_button.clicked.connect(self.save_profile)
        self.start_button = QPushButton("Start VR")
        self.start_button.clicked.connect(self.start)
        self.stop_button = QPushButton("Stop VR")
        self.stop_button.clicked.connect(self.stop)
        buttons.addWidget(self.save_button)
        buttons.addStretch()
        buttons.addWidget(self.start_button)
        buttons.addWidget(self.stop_button)
        layout.addLayout(buttons)
        self.status = QLabel("Ready. Start VR to choose a window or monitor.")
        layout.addWidget(self.status)
        self.logs = QPlainTextEdit()
        self.logs.setReadOnly(True)
        self.logs.document().setMaximumBlockCount(500)
        layout.addWidget(self.logs)
        self.setCentralWidget(central)
        self.select_profile(self.profile_choices.currentText())
        self.set_running(False)

    def select_profile(self, name):
        if name in self.profiles:
            self.profile_name.setText(name)
            self.loading_profile = True
            try:
                self.cuda.setChecked(self.profiles[name]["cuda"])
                for key, control in self.controls.items():
                    control.setValue(self.profiles[name][key])
            finally:
                self.loading_profile = False
            self.settings_changed()

    def save_profile(self):
        name = self.profile_name.text().strip()
        if not name or len(name) > 80:
            QMessageBox.warning(self, "Profile name", "Enter a name of 1 to 80 characters.")
            return
        profiles = {**self.profiles, name: {
            "cuda": self.cuda.isChecked(),
            **{key: control.value() for key, control in self.controls.items()},
        }}
        try:
            save_profiles(profiles)
        except OSError as error:
            QMessageBox.critical(self, "Save profile", str(error))
            return
        self.profiles = profiles
        if self.profile_choices.findText(name) < 0:
            self.profile_choices.addItem(name)
        self.profile_choices.setCurrentText(name)
        self.status.setText(f"Saved profile: {name}")

    def settings_changed(self):
        if not self.loading_profile and self.process.state() != QProcess.ProcessState.NotRunning:
            self.settings_timer.start(100)

    def write_runtime_settings(self):
        values = [self.controls[key].value() for key in RANGES]
        snapshot = "VRXL 1 " + " ".join(f"{value:.3f}" for value in values) + "\n"
        temporary = self.runtime_settings.with_suffix(".tmp")
        temporary.write_text(snapshot, encoding="ascii")
        os.replace(temporary, self.runtime_settings)

    def apply_runtime_settings(self):
        try:
            self.write_runtime_settings()
        except OSError as error:
            self.status.setText(f"Cannot apply live settings: {error}")
            self.logs.appendPlainText(f"Cannot apply live settings: {error}")

    def set_running(self, running):
        self.start_button.setEnabled(not running)
        self.stop_button.setEnabled(running)
        self.cuda.setEnabled(not running)

    def start(self):
        if not ENGINE.is_file():
            QMessageBox.critical(self, "Engine missing", f"Build the Linux engine first:\n{ENGINE}")
            return
        if self.cuda.isChecked() and not MODEL.is_file():
            QMessageBox.critical(self, "Model missing", f"Fetch the checked ZipDepth model first:\n{MODEL}")
            return
        try:
            self.write_runtime_settings()
        except OSError as error:
            QMessageBox.critical(self, "Settings", f"Cannot write live settings: {error}")
            return
        arguments = ["--until-stop", "--live", f"--settings={self.runtime_settings}"]
        if self.cuda.isChecked():
            arguments += ["--cuda", f"--model={MODEL}"]
        self.logs.appendPlainText("Starting: " + " ".join([str(ENGINE), *arguments]))
        self.process.setWorkingDirectory(str(ROOT))
        self.process.setProcessEnvironment(QProcessEnvironment.systemEnvironment())
        self.process.start(str(ENGINE), arguments)
        self.status.setText("Starting VR; choose a source in the ScreenCast dialog.")
        self.set_running(True)

    def stop(self):
        if self.process.state() == QProcess.ProcessState.NotRunning:
            return
        self.status.setText("Stopping VR…")
        self.process.terminate()
        self.kill_timer.start(5000)

    def force_stop(self):
        if self.process.state() != QProcess.ProcessState.NotRunning:
            self.logs.appendPlainText("Engine did not stop after SIGTERM; forcing shutdown.")
            self.process.kill()

    def read_output(self):
        content = bytes(self.process.readAllStandardOutput()).decode("utf-8", errors="replace")
        if content:
            self.logs.insertPlainText(content)
            self.logs.ensureCursorVisible()

    def process_error(self, error):
        self.logs.appendPlainText(f"Engine process error: {error.name}")
        if self.process.state() == QProcess.ProcessState.NotRunning:
            self.set_running(False)
            self.status.setText("Could not start VR. See log for details.")

    def finished(self, code, status):
        self.settings_timer.stop()
        self.kill_timer.stop()
        self.read_output()
        self.set_running(False)
        self.status.setText(f"VR stopped (exit {code}, {status.name}).")

    def closeEvent(self, event):
        if self.process.state() != QProcess.ProcessState.NotRunning:
            self.process.terminate()
            if not self.process.waitForFinished(3000):
                self.process.kill()
                self.process.waitForFinished(1000)
        self.session_directory.cleanup()
        super().closeEvent(event)


def main():
    app = QApplication(sys.argv)
    try:
        window = Controller()
    except RuntimeError as error:
        QMessageBox.critical(None, "VRX Linux", str(error))
        return 1
    window.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
