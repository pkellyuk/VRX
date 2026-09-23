#!/usr/bin/env python3
"""Early Linux session controller for the OpenXR diagnostic engine."""
import json
import os
from pathlib import Path
import sys

from PyQt6.QtCore import QProcess, QProcessEnvironment, QTimer
from PyQt6.QtWidgets import (
    QApplication, QCheckBox, QComboBox, QFormLayout, QHBoxLayout,
    QLabel, QLineEdit, QMainWindow, QMessageBox, QPushButton,
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


def load_profiles():
    if not PROFILES.exists():
        return {"Default": {"cuda": True}}
    try:
        document = json.loads(PROFILES.read_text(encoding="utf-8"))
        if document.get("version") != 1 or not isinstance(document.get("profiles"), dict):
            raise ValueError("unsupported profile format")
        return {str(name): {"cuda": bool(value["cuda"])}
                for name, value in document["profiles"].items() if isinstance(value, dict)}
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
            self.cuda.setChecked(self.profiles[name]["cuda"])

    def save_profile(self):
        name = self.profile_name.text().strip()
        if not name or len(name) > 80:
            QMessageBox.warning(self, "Profile name", "Enter a name of 1 to 80 characters.")
            return
        self.profiles[name] = {"cuda": self.cuda.isChecked()}
        try:
            save_profiles(self.profiles)
        except OSError as error:
            QMessageBox.critical(self, "Save profile", str(error))
            return
        if self.profile_choices.findText(name) < 0:
            self.profile_choices.addItem(name)
        self.profile_choices.setCurrentText(name)
        self.status.setText(f"Saved profile: {name}")

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
        arguments = ["--until-stop", "--live"]
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
