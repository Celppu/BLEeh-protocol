"""Compile and run the portable debouncer using host g++ where available."""
import shutil
import subprocess
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_active_low_bounce_and_retrigger():
    if not shutil.which("g++"):
        pytest.skip("g++ unavailable")
    build = ROOT / "build"  # inside the repo, gitignored
    build.mkdir(exist_ok=True)
    executable = build / "button_test"
    subprocess.run(
        ["g++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
         "-I", str(ROOT / "include"),
         str(ROOT / "tests" / "button_debounce_test.cpp"),
         "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)
