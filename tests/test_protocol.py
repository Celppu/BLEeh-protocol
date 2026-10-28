"""BLEeh protocol tests: C++ and Python implementations against test-vectors/v1.json."""
import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

import bleeh  # noqa: E402

V = json.loads((ROOT / "test-vectors" / "v1.json").read_text(encoding="utf-8"))


def test_uuids():
    assert V["uuids"] == dict(service=bleeh.SERVICE_UUID, rc=bleeh.RC_UUID,
                              telemetry=bleeh.TELEMETRY_UUID, capabilities=bleeh.CAPS_UUID)


def test_rc_frame():
    rc = V["rc"]
    frame = bleeh.encode_rc(rc["channels"], rc["sequence"], rc["flags"])
    assert frame.hex() == rc["hex"]
    assert len(frame) == bleeh.RC_FRAME_LEN == 25
    flags, seq, chans = bleeh.decode_rc(frame + b"\x00\x00")  # longer writes are allowed
    assert (flags, seq, chans) == (rc["flags"], rc["sequence"], tuple(rc["channels"]))


def test_telemetry():
    v = V["telemetry"]
    data = bytes.fromhex(v["hex"])
    t = bleeh.decode_telemetry(data)
    assert t.sequence == v["sequence"]
    assert t.link == v["link"]
    assert t.battery == v["battery"]
    assert t.flight_mode == v["flight_mode"]
    assert t.vario_cms == v["vario_cms"]
    assert t.unknown == {int(k): bytes.fromhex(h) for k, h in v["unknown"].items()}
    with pytest.raises(ValueError):
        bleeh.decode_telemetry(data[:-1])


def test_capabilities():
    v = V["capabilities"]
    c = bleeh.decode_capabilities(bytes.fromhex(v["hex"]))
    assert (c.protocol_version, c.channel_count, c.features, c.telemetry_types, c.name) == \
        (v["protocol_version"], v["channel_count"], v["features"], v["telemetry_types"], v["name"])


def test_adv_manufacturer():
    v = V["adv_manufacturer"]
    data = bytes.fromhex(v["hex"])
    company = int.from_bytes(data[:2], "little")
    assert bleeh.decode_adv_manufacturer(company, data[2:]) == (v["version"], v["flags"])
    assert bleeh.decode_adv_manufacturer(company, b"LL\x01\x01") is None


def test_cpp_matches_vectors():
    if not shutil.which("g++"):
        pytest.skip("g++ unavailable")
    src = ROOT / "arduino" / "src"
    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    exe = build / "protocol_test"
    subprocess.run(["g++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(src),
                    str(ROOT / "tests" / "protocol_test.cpp"),
                    str(src / "BLEehProtocol.cpp"), "-o", str(exe)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    frames = dict(line.split() for line in out.splitlines())
    assert frames == {name: V[name]["hex"] for name in ("rc", "telemetry", "capabilities", "adv_manufacturer")}
