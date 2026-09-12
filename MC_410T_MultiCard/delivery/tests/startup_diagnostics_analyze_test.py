"""Synthetic regression tests for START-send -> ingress -> SourceCore joins."""
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
SPEC = importlib.util.spec_from_file_location(
    "startup_diagnostics_analyze", TOOLS / "startup_diagnostics_analyze.py"
)
assert SPEC and SPEC.loader
ANALYZER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ANALYZER
SPEC.loader.exec_module(ANALYZER)


def ip_value(value: str) -> int:
    return int.from_bytes(socket.inet_aton(value), "little")


def trace_record(
    sequence: int,
    tick: int,
    session: int,
    correlation: int = 0,
    *,
    card: int = 0,
    stage: int = 0,
    reason: int = 0,
    trigger: int = 0,
    packet: int = 0,
    length: int = 0,
    value: int = 0,
    local_port: int = 8001,
    source_port: int = 8080,
    source_ip: str = "127.0.0.2",
) -> bytes:
    header = struct.pack("<HH", packet, trigger)
    return TRACE_RECORD.pack(
        sequence, tick, session, correlation, 1, ip_value(source_ip), value,
        local_port, source_port, length, trigger, packet, card, stage, reason,
        *header, 2,
    )


def start_records(session: int, tick: int = 100, failed_cards: set[int] | None = None,
                  sequence_start: int = 1) -> list[bytes]:
    failed_cards = failed_cards or set()
    records = []
    for card in range(4):
        records.append(trace_record(
            sequence_start + card, tick + card * 10, session, card=card, stage=6,
            reason=3, packet=1, length=57 if card in failed_cards else 58,
            value=100 if card in failed_cards else 0,
            source_ip=f"127.0.0.{card + 2}",
        ))
    return records


def raw_and_decision(
    sequence: int, tick: int, session: int, correlation: int, *, card: int,
    trigger: int, packet: int, reason: int,
) -> list[bytes]:
    return [
        trace_record(sequence, tick, session, correlation, card=card, stage=1,
                     trigger=trigger, packet=packet, length=1444,
                     local_port=8001 + card, source_port=8080,
                     source_ip=f"127.0.0.{card + 2}"),
        trace_record(sequence + 1, tick + 1, session, correlation, card=card,
                     stage=2, reason=reason, trigger=trigger, packet=packet,
                     length=0, value=1, local_port=8001 + card,
                     source_ip=f"127.0.0.{card + 2}"),
    ]


def write_fixture(root: Path, records: list[bytes], *, queue_dropped: int = 0,
                  trace_incomplete: bool = False) -> None:
    root.mkdir(parents=True, exist_ok=True)
    (root / "run-config.json").write_text(json.dumps({
        "schemaVersion": 2, "cards": 4, "dataPort": 8001,
        "feedbackPort": 8000, "samples": 1440, "bits": 32,
    }), encoding="utf-8")
    (root / "trace-0.bin").write_bytes(b"".join(records))
    (root / "trace-summary.json").write_text(json.dumps({
        "schemaVersion": 2, "recordBytes": 64, "recordsIssued": len(records),
        "recordsWritten": len(records), "queueDropped": queue_dropped,
        "writeFailed": False, "traceIncomplete": trace_incomplete,
    }), encoding="utf-8")


def analyze(root: Path) -> dict:
    output = root / "analysis.json"
    result = ANALYZER.run(root, None, None, output)
    assert json.loads(output.read_text(encoding="utf-8")) == result
    assert "startAdmissionEvidence" in result
    assert any(row["evidence"] == "application_start_admission"
               for row in result["evidenceConclusionMatrix"])
    return result


def session(result: dict, number: int) -> dict:
    return next(item for item in result["startAdmissionEvidence"]["sessions"]
                if item["session"] == number)


def main() -> None:
    base = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(tempfile.mkdtemp())
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="startup-admission-", dir=base) as temp:
        root = Path(temp)

        positive = root / "positive"
        records = start_records(1)
        records += raw_and_decision(5, 105, 1, 500, card=0, trigger=100, packet=0, reason=2)
        records += raw_and_decision(7, 200, 1, 501, card=0, trigger=101, packet=0, reason=0)
        write_fixture(positive, records)
        result = analyze(positive)
        assert result["startAdmissionEvidence"]["status"] == "application_start_gate_drop_observed"
        positive_session = session(result, 1)
        assert positive_session["classification"] == "application_start_gate_drop_observed"
        card0 = next(item for item in positive_session["perCard"] if item["card"] == 0)
        assert card0["matchedDisabledInStartSpanCount"] == 1
        assert card0["firstDisabledIngress"]["correlation"] == 500
        assert card0["firstAcceptedIngress"]["correlation"] == 501

        accepted = root / "accepted-only"
        records = start_records(2)
        records += raw_and_decision(5, 115, 2, 600, card=0, trigger=100, packet=0, reason=0)
        write_fixture(accepted, records)
        result = analyze(accepted)
        assert result["startAdmissionEvidence"]["status"] == "no_application_start_gate_drop_observed"
        assert session(result, 2)["classification"] == "no_application_start_gate_drop_observed"

        incomplete = root / "incomplete"
        records = start_records(3)
        records += raw_and_decision(5, 115, 3, 700, card=0, trigger=100, packet=0, reason=0)
        write_fixture(incomplete, records, queue_dropped=1)
        result = analyze(incomplete)
        assert result["application"]["traceIncomplete"]
        assert result["startAdmissionEvidence"]["status"] == "trace_incomplete"
        assert session(result, 3)["negativeConclusionDowngraded"]
        assert session(result, 3)["classification"] != "no_application_start_gate_drop_observed"

        correlation = root / "correlation-over-number"
        records = start_records(4, 100)
        records += raw_and_decision(5, 105, 4, 800, card=0, trigger=700, packet=0, reason=2)
        records += start_records(5, 500, sequence_start=7)
        records += raw_and_decision(11, 515, 5, 801, card=0, trigger=700, packet=0, reason=0)
        write_fixture(correlation, records)
        result = analyze(correlation)
        assert session(result, 4)["classification"] == "application_start_gate_drop_observed"
        assert session(result, 5)["classification"] == "no_application_start_gate_drop_observed"
        assert session(result, 4)["perCard"][0]["firstDisabledIngress"]["correlation"] == 800
        assert session(result, 5)["perCard"][0]["firstAcceptedIngress"]["correlation"] == 801

        send_failure = root / "control-send-failure"
        records = start_records(6, failed_cards={0})
        write_fixture(send_failure, records)
        result = analyze(send_failure)
        assert result["startAdmissionEvidence"]["status"] == "control_send_failed"
        failed = session(result, 6)
        assert failed["classification"] == "control_send_failed"
        assert failed["sendFailures"][0]["card"] == 0
        assert failed["sendFailures"][0]["error"] == 100

    print("PASS startup diagnostics START admission fixtures: positive, accepted-only, incomplete, correlation, send-failure")


if __name__ == "__main__":
    main()
