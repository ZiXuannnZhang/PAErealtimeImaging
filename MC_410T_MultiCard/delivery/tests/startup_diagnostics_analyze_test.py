"""Synthetic regression coverage for START Fence evidence joins."""
from __future__ import annotations

import importlib.util
import json
import socket
import struct
import sys
import tempfile
from pathlib import Path

TRACE_RECORD = struct.Struct("<4Q3I5Hh6BH")
assert TRACE_RECORD.size == 64

TOOLS = Path(__file__).resolve().parents[1] / "tools"
SPEC = importlib.util.spec_from_file_location("startup_diagnostics_analyze", TOOLS / "startup_diagnostics_analyze.py")
assert SPEC and SPEC.loader
ANALYZER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ANALYZER
SPEC.loader.exec_module(ANALYZER)


def ip_value(value: str) -> int:
    return int.from_bytes(socket.inet_aton(value), "little")


def record(sequence: int, tick: int, session: int, correlation: int = 0, *, card: int = 0,
           stage: int = 0, reason: int = 0, trigger: int = 0, packet: int = 0,
           length: int = 0, value: int = 0) -> bytes:
    return TRACE_RECORD.pack(
        sequence, tick, session, correlation, 1, ip_value(f"127.0.0.{card + 2}"),
        value, 8001 + card, 8080, length, trigger, packet, card, stage, reason,
        packet & 0xff, packet >> 8, trigger & 0xff, trigger >> 8, 2,
    )


def starts(session: int, failed_card: int | None = None, sequence_start: int = 1,
           tick_base: int = 110) -> list[bytes]:
    return [record(sequence_start + card, tick_base + card * 10, session, card=card, stage=6, reason=3,
                   length=0 if card == failed_card else 58, value=100 if card == failed_card else 0,
                   packet=1) for card in range(4)]


def packet_observations(sequence: int, session: int, correlation: int, *, card: int, trigger: int,
                        packet: int, decisions: list[int], raw_tick: int = 150,
                        decision_tick: int = 151) -> list[bytes]:
    """One stage-1 ingress followed by many stage-2 observations."""
    rows = [record(sequence, raw_tick, session, correlation, card=card, stage=1,
                   trigger=trigger, packet=packet, length=1444)]
    rows.extend(record(sequence + index + 1, decision_tick + index, session, correlation,
                       card=card, stage=2, trigger=trigger, packet=packet, reason=decision)
                for index, decision in enumerate(decisions))
    return rows


def write_fixture(root: Path, rows: list[bytes], *, dropped: int = 0) -> None:
    root.mkdir(parents=True, exist_ok=True)
    (root / "run-config.json").write_text(json.dumps({"schemaVersion": 2, "cards": 4, "dataPort": 8001}), encoding="utf-8")
    (root / "trace-0.bin").write_bytes(b"".join(rows))
    (root / "trace-summary.json").write_text(json.dumps({
        "schemaVersion": 2, "recordBytes": 64, "recordsIssued": len(rows),
        "recordsWritten": len(rows), "queueDropped": dropped, "writeFailed": False,
        "traceIncomplete": False,
    }), encoding="utf-8")


def analyze(root: Path) -> dict:
    output = root / "analysis.json"
    result = ANALYZER.run(root, None, None, output)
    assert json.loads(output.read_text(encoding="utf-8")) == result
    return result


def classification(result: dict, session: int) -> str:
    row = next(item for item in result["startAdmissionEvidence"]["sessions"] if item["session"] == session)
    return row["classification"]


def main() -> None:
    base = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(tempfile.mkdtemp())
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="startup-fence-", dir=base) as temp:
        root = Path(temp)

        clean = root / "clean"
        rows = starts(1)
        rows += packet_observations(5, 1, 500, card=0, trigger=100, packet=0,
                                    decisions=[30, 31, 0], raw_tick=100, decision_tick=101)
        write_fixture(clean, rows)
        result = analyze(clean)
        assert classification(result, 1) == "application_start_fence_clean"
        assert result["startAdmissionEvidence"]["summary"]["heldIngressCount"] == 1
        assert result["startAdmissionEvidence"]["summary"]["releasedIngressCount"] == 1
        assert result["startAdmissionEvidence"]["summary"]["missingStage2JoinCount"] == 0
        assert result["startAdmissionEvidence"]["sessions"][0]["rawIngressAfterBoundaryCount"] == 0

        gate_drop = root / "gate-drop"
        write_fixture(gate_drop, starts(2) + packet_observations(5, 2, 600, card=0, trigger=100,
                                                                  packet=0, decisions=[2]))
        assert classification(analyze(gate_drop), 2) == "application_start_gate_drop_observed"

        failed = root / "failed"
        rows = starts(3, failed_card=2)
        rows += packet_observations(5, 3, 700, card=0, trigger=100, packet=0, decisions=[32])
        write_fixture(failed, rows)
        assert classification(analyze(failed), 3) == "application_start_fence_failed"

        incomplete = root / "incomplete"
        write_fixture(incomplete, starts(4) + packet_observations(5, 4, 800, card=0, trigger=700,
                                                                   packet=0, decisions=[0]), dropped=1)
        assert classification(analyze(incomplete), 4) == "application_start_fence_inconclusive"

        prestart = root / "pre-start"
        write_fixture(prestart, starts(7) + packet_observations(5, 7, 850, card=0, trigger=701,
                                                                  packet=0, decisions=[29], raw_tick=100))
        result = analyze(prestart)
        assert result["startAdmissionEvidence"]["summary"]["preStartDiscardCount"] == 1
        assert result["startAdmissionEvidence"]["summary"]["missingStage2JoinCount"] == 0

        correlation = root / "session-correlation"
        rows = starts(5) + packet_observations(5, 5, 900, card=0, trigger=700, packet=0, decisions=[0])
        rows += starts(6, sequence_start=7) + packet_observations(11, 6, 900, card=0,
                                                                    trigger=700, packet=0, decisions=[2])
        write_fixture(correlation, rows)
        result = analyze(correlation)
        assert classification(result, 5) == "application_start_fence_clean"
        assert classification(result, 6) == "application_start_gate_drop_observed"

    print("PASS startup diagnostics START Fence fixtures: clean, gate-drop, failed, incomplete, session-correlation")


if __name__ == "__main__":
    main()
