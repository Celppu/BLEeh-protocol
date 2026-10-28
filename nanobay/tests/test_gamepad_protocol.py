import sys
from pathlib import Path
import struct
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from protocol import ChannelAssembler, PACKET, normalized


def packet(seq, bank, flags, values):
    return struct.pack("<HBB8H", seq, bank, flags, *values)


def test_format_length():
    assert PACKET.size == 20


def test_two_banks_are_assembled():
    assembler = ChannelAssembler()
    assert assembler.feed(packet(11, 1, 1, range(8, 16))) is None
    frame = assembler.feed(packet(11, 0, 1, range(8)))
    assert frame.sequence == 11
    assert frame.channels == tuple(range(16))
    assert frame.fresh is True
    assert frame.pairing is False


def test_mismatch_does_not_complete_frame():
    assembler = ChannelAssembler()
    assert assembler.feed(packet(11, 0, 1, [992]*8)) is None
    assert assembler.feed(packet(12, 1, 0, [1000]*8)) is None


def test_stale_and_pairing_flags():
    assembler = ChannelAssembler()
    assembler.feed(packet(1, 0, 2, [992]*8))
    frame = assembler.feed(packet(1, 1, 2, [992]*8))
    assert not frame.fresh
    assert frame.pairing


def test_normalization():
    assert normalized(172) == -1
    assert normalized(992) == 0
    assert normalized(1811) == 1
    assert -1 <= normalized(0) <= 1
