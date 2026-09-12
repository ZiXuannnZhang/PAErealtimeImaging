"""Build the startup evidence table from application and system-capture artifacts.

This is deliberately a read-only analyzer.  It reuses the binary parsers in
``paimage_trace_analyze.py`` and adds startup-specific fields without joining
16-bit trigger/packet numbers across independent rounds.
"""
from __future__ import annotations

import argparse
import json
import math
import socket
import struct
import statistics
import sys
import tempfile
import zipfile
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

from paimage_trace_analyze import (  # noqa: E402
    LOOPLOG,
    RECORD,
    _looplog,
    _pcapng_packets,
    _resolve_trace,
    analyze as analyze_trace,
)


def read_json(path: Path, default: Any = None) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError, TypeError):
        return default


def trace_files(root: Path) -> list[Path]:
    def key(path: Path) -> tuple[int, str]:
        try:
            return (int(path.stem.rsplit("-", 1)[-1]), path.name)
        except ValueError:
            return (0, path.name)

    return sorted(root.glob("trace-*.bin"), key=key)


def ip_from_uint32(value: int) -> str:
    try:
        return socket.inet_ntoa(int(value).to_bytes(4, "little", signed=False))
    except (OverflowError, OSError):
        return "unknown"


def percentiles(values: Iterable[float]) -> dict[str, float | None]:
    items = sorted(float(v) for v in values)
    if not items:
        return {"min": None, "median": None, "p95": None, "max": None}
    return {
        "min": items[0],
        "median": statistics.median(items),
        "p95": items[min(len(items) - 1, math.ceil(len(items) * 0.95) - 1)],
        "max": items[-1],
    }


DECISION_NAMES = {
    0: "Accepted", 1: "Short", 2: "Disabled", 3: "RecentTrigger",
    4: "Duplicate", 5: "OffsetOutside", 6: "Complete",
    7: "TriggerSwitch", 8: "Timeout", 9: "StartupIdleClear",
    10: "StartupOverflow", 11: "StartupConfirmed", 12: "SyncExpired",
    13: "StopTruncated", 14: "StartupBuffered", 15: "CardOutput",
    16: "SyncOutput", 17: "InvalidCard", 18: "StartupOverflowDiscard",
    19: "StopBufferedDiscard", 20: "ListenerActiveDiscard",
    21: "ListenerBufferedDiscard", 22: "ListenerPendingSyncDiscard",
    23: "StartPendingSyncDiscard", 24: "StartActiveDiscard",
    25: "StartupActiveDiscard", 26: "StartupPendingSyncDiscard",
    27: "CompleteStartPendingSyncDiscard", 28: "CompleteStartActiveDiscard",
}
DIRECT_SOURCE_DECISIONS = frozenset({0, 1, 2, 3, 4, 5, 17})


def decision_name(reason: int) -> str:
    return DECISION_NAMES.get(int(reason), f"Decision{int(reason)}")


def parse_application(root: Path) -> dict[str, Any]:
    metadata = read_json(root / "run-config.json", {}) or {}
    summary = read_json(root / "trace-summary.json", {}) or {}
    try:
        data_port = int(metadata.get("dataPort", 18001) or 18001)
    except (TypeError, ValueError):
        data_port = 18001
    try:
        card_count = int(metadata.get("cards", 4) or 4)
    except (TypeError, ValueError):
        card_count = 4
    target_cards: dict[str, int] = {}
    for target in metadata.get("targets", []) or []:
        if isinstance(target, dict) and target.get("ip") is not None and target.get("card") is not None:
            target_cards[str(target["ip"])] = int(target["card"])
    ingress: list[dict[str, Any]] = []
    all_times: list[int] = []
    stage2_rejects: list[dict[str, Any]] = []
    stage2_decisions: list[dict[str, Any]] = []
    start_sends: list[dict[str, Any]] = []
    sequences: set[int] = set()
    malformed = 0
    records = 0
    for path in trace_files(root):
        data = path.read_bytes()
        malformed += int(len(data) % RECORD.size != 0)
        usable = len(data) // RECORD.size * RECORD.size
        for values in RECORD.iter_unpack(data[:usable]):
            (sequence, tick, session, correlation, thread_id, source_ip,
             value, local_port, source_port, length, trigger, packet, card,
             stage, reason, header, schema) = values
            records += 1
            if sequence:
                sequences.add(sequence)
            if tick:
                all_times.append(tick)
            if stage == 1:
                if len(header) >= 4:
                    header_packet = int.from_bytes(header[0:2], "little")
                    header_trigger = int.from_bytes(header[2:4], "little")
                else:
                    header_packet = packet
                    header_trigger = trigger
                source_ipv4 = ip_from_uint32(source_ip)
                observed_card = int(card)
                if observed_card < 0 and data_port <= int(local_port) < data_port + card_count:
                    observed_card = int(local_port) - data_port
                if observed_card < 0:
                    try:
                        feedback_port = int(metadata.get("feedbackPort", 0) or 0)
                    except (TypeError, ValueError):
                        feedback_port = 0
                    if feedback_port and int(local_port) == feedback_port:
                        observed_card = target_cards.get(source_ipv4, -1)
                ingress.append({
                    "sequence": sequence,
                    "steadyNs": tick,
                    "session": session,
                    "correlation": correlation,
                    "threadId": thread_id,
                    "card": observed_card,
                    "traceCard": card,
                    "sourceIPv4": source_ipv4,
                    "localPort": local_port,
                    "sourcePort": source_port,
                    "trigger": trigger if trigger is not None else header_trigger,
                    "packet": packet if packet is not None else header_packet,
                    "headerTrigger": header_trigger,
                    "headerPacket": header_packet,
                    "length": length,
                    "schema": schema,
                    "stage": stage,
                    "reason": reason,
                })
            elif stage == 2:
                stage2_decisions.append({
                    "sequence": sequence, "steadyNs": tick, "session": session,
                    "correlation": correlation, "card": card, "trigger": trigger,
                    "packet": packet, "reason": reason, "decision": decision_name(reason),
                    "count": value,
                })
                if reason not in (6, 7, 8):
                    stage2_rejects.append({
                        "steadyNs": tick, "session": session, "card": card,
                        "trigger": trigger, "reason": reason, "count": value,
                    })
            elif stage == 6 and reason == 3:
                start_sends.append({
                    "sequence": sequence, "steadyNs": tick, "session": session,
                    "correlation": correlation, "card": card,
                    "sourceIPv4": ip_from_uint32(source_ip), "sourceIPv4Value": source_ip,
                    "length": length, "error": value, "reason": reason,
                    "commandPacket": packet, "stage": stage,
                })

    issued = int(summary.get("recordsIssued", 0) or 0)
    missing_stored_records = max(0, max(issued, max(sequences, default=0)) - len(sequences))
    ingress.sort(key=lambda item: (item["steadyNs"], item["sequence"]))

    by_card: dict[int, list[dict[str, Any]]] = defaultdict(list)
    by_card_trigger: dict[tuple[int, int], list[dict[str, Any]]] = defaultdict(list)
    for item in ingress:
        by_card[int(item.get("card", -1))].append(item)
        by_card_trigger[(int(item.get("card", -1)), int(item.get("trigger", 0)))].append(item)

    first_numbers = []
    card_first_ns: dict[str, int | None] = {}
    card_trigger_first: dict[int, dict[int, int]] = defaultdict(dict)
    interval_by_card: dict[str, dict[str, Any]] = {}
    for card, items in sorted(by_card.items()):
        if not items:
            continue
        first = items[0]
        card_first_ns[str(card)] = first["steadyNs"]
        for item in items:
            trig = int(item["trigger"])
            card_trigger_first.setdefault(card, {}).setdefault(trig, item["steadyNs"])
        deltas = [b["steadyNs"] - a["steadyNs"] for a, b in zip(items, items[1:])
                  if b["steadyNs"] >= a["steadyNs"]]
        interval_by_card[str(card)] = {
            "count": len(deltas),
            "nanoseconds": percentiles(deltas),
            "milliseconds": percentiles([value / 1e6 for value in deltas]),
        }
        first_numbers.append({
            "card": card,
            "firstSequence": first["sequence"],
            "firstSteadyNs": first["steadyNs"],
            "firstSourceIPv4": first["sourceIPv4"],
            "firstLocalPort": first["localPort"],
            "firstSourcePort": first["sourcePort"],
            "firstTrigger": first["trigger"],
            "firstPacket": first["packet"],
            "firstLength": first["length"],
        })

    card_range = None
    if card_first_ns:
        values = list(card_first_ns.values())
        card_range = {
            "minSteadyNs": min(values), "maxSteadyNs": max(values),
            "rangeNs": max(values) - min(values),
            "rangeMs": (max(values) - min(values)) / 1e6,
        }

    rhythm = {}
    for card, trigger_times in sorted(card_trigger_first.items()):
        times = [trigger_times[key] for key in sorted(trigger_times)]
        deltas = [b - a for a, b in zip(times, times[1:]) if b >= a]
        near_25 = [value for value in deltas if 20e6 <= value <= 30e6]
        rhythm[str(card)] = {
            "triggerCount": len(times),
            "intervalCount": len(deltas),
            "intervalMs": percentiles([value / 1e6 for value in deltas]),
            "near25msCount": len(near_25),
            "near25msRatio": (len(near_25) / len(deltas)) if deltas else None,
            "status": "observed" if near_25 else ("not_enough_data" if not deltas else "not_25ms"),
        }

    if card_first_ns:
        consistency = {
            "cardCountObserved": len(card_first_ns),
            "expectedCardCount": int(metadata.get("cards", 0) or 0) or None,
            "firstIngressRange": card_range,
            "startupFirstNumbers": first_numbers,
            "status": "consistent_within_25ms"
            if card_range and card_range["rangeMs"] <= 25 else "spread_exceeds_25ms_or_unknown",
        }
    else:
        consistency = {
            "cardCountObserved": 0, "expectedCardCount": int(metadata.get("cards", 0) or 0) or None,
            "firstIngressRange": None, "startupFirstNumbers": [], "status": "no_application_ingress",
        }

    earliest = min(all_times, default=None)
    latest = max(all_times, default=None)
    per_second: dict[int, dict[str, Any]] = {}
    for item in ingress:
        if earliest is None:
            continue
        bucket = max(0, int((item["steadyNs"] - earliest) // 1_000_000_000))
        row = per_second.setdefault(bucket, {
            "secondFromFirstIngress": bucket,
            "ingressPackets": 0,
            "shortIngressPackets": 0,
            "sourceRejectCount": 0,
            "diagnosticWriteDropped": None,
            "diagnosticQueueDropped": None,
            "loopGapCount": 0,
            "maxLoopGapNs": None,
        })
        row["ingressPackets"] += 1
        if int(item.get("length", 0)) < 4:
            row["shortIngressPackets"] += 1
    for item in stage2_rejects:
        if earliest is None:
            continue
        bucket = max(0, int((item["steadyNs"] - earliest) // 1_000_000_000))
        row = per_second.setdefault(bucket, {
            "secondFromFirstIngress": bucket, "ingressPackets": 0,
            "shortIngressPackets": 0, "sourceRejectCount": 0,
            "diagnosticWriteDropped": None, "diagnosticQueueDropped": None,
            "loopGapCount": 0, "maxLoopGapNs": None,
        })
        row["sourceRejectCount"] += int(item.get("count", 0) or 0)

    loop = _looplog(root)
    loop_records = []
    for path in sorted(root.glob("looplog-*.bin")):
        data = path.read_bytes()
        for values in LOOPLOG.iter_unpack(data[:len(data) // LOOPLOG.size * LOOPLOG.size]):
            seq, tick, span, session, thread_id, kind, flags, v0, v1, v2, v3, pa, pb, pc = values
            loop_records.append({"sequence": seq, "timeNs": tick, "spanNs": span,
                                 "kind": kind, "session": session, "threadId": thread_id,
                                 "value0": v0, "value1": v1, "payloadA": pa})
            if earliest is not None:
                bucket = max(0, int((tick - earliest) // 1_000_000_000))
                row = per_second.setdefault(bucket, {
                    "secondFromFirstIngress": bucket, "ingressPackets": 0,
                    "shortIngressPackets": 0, "sourceRejectCount": 0,
                    "diagnosticWriteDropped": None, "diagnosticQueueDropped": None,
                    "loopGapCount": 0, "maxLoopGapNs": None,
                })
                if kind == 1:
                    row["loopGapCount"] += 1
                    row["maxLoopGapNs"] = max(row["maxLoopGapNs"] or 0, int(span))

    trace_queue_dropped = summary.get("queueDropped")
    trace_write_failed = summary.get("writeFailed")
    for row in per_second.values():
        row["diagnosticQueueDropped"] = trace_queue_dropped
        row["diagnosticWriteDropped"] = trace_write_failed

    diagnostics = {
        "perSecond": [per_second[key] for key in sorted(per_second)],
        "totals": {
            "applicationTraceRecords": records,
            "applicationTraceMissingStoredRecords": missing_stored_records,
            "traceSummaryQueueDropped": trace_queue_dropped,
            "traceSummaryWriteFailed": trace_write_failed,
            "loopQueueDropped": loop.get("summary", {}).get("queueDropped"),
            "loopBudgetOmitted": loop.get("summary", {}).get("budgetOmitted"),
            "loopRetentionEvicted": loop.get("summary", {}).get("retentionEvicted"),
            "sourceRejectIndicators": sum(int(item.get("count", 0) or 0) for item in stage2_rejects),
        },
        "beforeAfter": {
            "firstTwoSeconds": [row for row in per_second.values() if row["secondFromFirstIngress"] < 2],
            "afterFirstTwoSeconds": [row for row in per_second.values() if row["secondFromFirstIngress"] >= 2],
            "classification": "observed indicators only; absence of an indicator is not proof of no loss",
        },
    }

    return {
        "root": str(root), "metadata": metadata, "summary": summary,
        "records": records, "ingressPackets": ingress, "firstNumbers": first_numbers,
        "perPacketIngress": ingress, "packetIntervals": interval_by_card,
        "startupConsistency": consistency, "rhythm25ms": rhythm,
        "earliestSteadyNs": earliest, "latestSteadyNs": latest,
        "missingStoredRecords": missing_stored_records, "malformedRecords": malformed,
        "traceIncomplete": bool(
            not summary
            or missing_stored_records
            or malformed
            or summary.get("traceIncomplete", False)
            or int(summary.get("queueDropped", 0) or 0) > 0
            or int(summary.get("writerUnwritten", 0) or 0) > 0
            or bool(summary.get("writeFailed", False))
        ),
        "sourceRejectIndicators": stage2_rejects,
        "stage2Decisions": stage2_decisions,
        "startSends": start_sends,
        "loop": loop, "loopRecords": loop_records, "diagnostics": diagnostics,
    }


def _ingress_reference(item: dict[str, Any]) -> dict[str, Any]:
    return {
        "sequence": item.get("sequence"), "steadyNs": item.get("steadyNs"),
        "session": item.get("session"), "card": item.get("card"),
        "correlation": item.get("correlation"), "trigger": item.get("trigger"),
        "packet": item.get("packet"), "length": item.get("length"),
    }


def start_admission_evidence(application: dict[str, Any]) -> dict[str, Any]:
    """Join START sends to raw ingress and the matching SourceCore decision.

    The join deliberately uses the application-provided ingress correlation and
    the measurement session.  Trigger/packet numbers are retained as evidence,
    but are never used as the identity of a packet across sessions.
    """
    starts = sorted(
        application.get("startSends", []),
        key=lambda item: (int(item.get("steadyNs", 0)), int(item.get("sequence", 0))),
    )
    ingress = [
        item for item in application.get("ingressPackets", [])
        if int(item.get("card", -1)) >= 0
    ]
    decisions = application.get("stage2Decisions", [])
    stage2_by_key: dict[tuple[int, int, int], list[dict[str, Any]]] = defaultdict(list)
    for item in decisions:
        key = (int(item.get("session", 0)), int(item.get("correlation", 0)),
               int(item.get("card", -1)))
        stage2_by_key[key].append(item)
    for values in stage2_by_key.values():
        values.sort(key=lambda item: (int(item.get("steadyNs", 0)),
                                      int(item.get("sequence", 0))))

    def join(raw: dict[str, Any]) -> tuple[dict[str, Any] | None, int, bool]:
        key = (int(raw.get("session", 0)), int(raw.get("correlation", 0)),
               int(raw.get("card", -1)))
        candidates = stage2_by_key.get(key, [])
        exact = [item for item in candidates
                 if int(item.get("trigger", 0)) == int(raw.get("trigger", 0))
                 and int(item.get("packet", 0)) == int(raw.get("packet", 0))]
        direct = [item for item in exact
                  if int(item.get("reason", -1)) in DIRECT_SOURCE_DECISIONS]
        if direct:
            return direct[0], len(candidates), True
        if exact:
            return exact[0], len(candidates), False
        return None, len(candidates), False

    by_session_starts: dict[int, list[dict[str, Any]]] = defaultdict(list)
    by_session_ingress: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for item in starts:
        by_session_starts[int(item.get("session", 0))].append(item)
    for item in ingress:
        by_session_ingress[int(item.get("session", 0))].append(item)

    metadata = application.get("metadata", {}) or {}
    try:
        metadata_cards = int(metadata.get("cards", 0) or 0)
    except (TypeError, ValueError):
        metadata_cards = 0
    observed_cards = {
        int(item.get("card", -1)) for item in starts + ingress
        if int(item.get("card", -1)) >= 0
    }
    expected_cards = metadata_cards or (max(observed_cards) + 1 if observed_cards else 0)
    session_ids = sorted(set(by_session_starts) | set(by_session_ingress))
    sessions: list[dict[str, Any]] = []
    classification_counts: dict[str, int] = defaultdict(int)

    for session in session_ids:
        session_starts = by_session_starts.get(session, [])
        session_ingress = sorted(
            by_session_ingress.get(session, []),
            key=lambda item: (int(item.get("steadyNs", 0)), int(item.get("sequence", 0))),
        )
        joined = []
        for raw in session_ingress:
            selected, candidate_count, direct = join(raw)
            joined.append((raw, selected, candidate_count, direct))

        start_ns = min((int(item["steadyNs"]) for item in session_starts), default=None)
        last_start_ns = max((int(item["steadyNs"]) for item in session_starts), default=None)
        send_order = [int(item.get("card", -1)) for item in session_starts]
        failures = [
            {
                "card": item.get("card"), "steadyNs": item.get("steadyNs"),
                "sequence": item.get("sequence"), "bytes": item.get("length"),
                "error": item.get("error"),
            }
            for item in session_starts
            if int(item.get("length", 0) or 0) != 58 or int(item.get("error", 0) or 0) != 0
        ]
        per_card: list[dict[str, Any]] = []
        session_raw_after_start = 0
        session_raw_in_span = 0
        session_matched = 0
        session_direct = 0
        session_missing = 0
        session_disabled = 0
        session_accepted = 0

        card_ids = set(range(expected_cards)) | {
            int(item.get("card", -1)) for item in session_starts + session_ingress
            if int(item.get("card", -1)) >= 0
        }
        for card in sorted(card_ids):
            card_starts = [item for item in session_starts if int(item.get("card", -1)) == card]
            card_start_ns = min((int(item["steadyNs"]) for item in card_starts), default=None)
            card_raw = [item for item in joined if int(item[0].get("card", -1)) == card]
            after_start = [item for item in card_raw
                           if card_start_ns is not None
                           and int(item[0].get("steadyNs", 0)) >= card_start_ns]
            in_start_span = [item for item in card_raw
                             if start_ns is not None and last_start_ns is not None
                             and start_ns <= int(item[0].get("steadyNs", 0)) <= last_start_ns]

            def direct_decision(item: tuple[dict[str, Any], dict[str, Any] | None, int, bool]) -> int | None:
                selected = item[1]
                return int(selected["reason"]) if item[3] and selected is not None else None

            def first_with_reason(items: list[tuple[dict[str, Any], dict[str, Any] | None, int, bool]],
                                  reason: int) -> dict[str, Any] | None:
                for raw, selected, _, direct in items:
                    if direct and selected is not None and int(selected.get("reason", -1)) == reason:
                        return _ingress_reference(raw) | {
                            "decision": decision_name(reason), "reason": reason,
                        }
                return None

            raw_after_count = len(after_start)
            raw_span_count = len(in_start_span)
            matched_after = sum(1 for _, _, candidates, _ in after_start if candidates)
            direct_after = sum(1 for _, _, _, direct in after_start if direct)
            missing_after = raw_after_count - direct_after
            disabled_after = sum(1 for item in after_start if direct_decision(item) == 2)
            accepted_after = sum(1 for item in after_start if direct_decision(item) == 0)
            matched_span = sum(1 for _, _, candidates, _ in in_start_span if candidates)
            direct_span = sum(1 for _, _, _, direct in in_start_span if direct)
            missing_span = raw_span_count - direct_span
            disabled_span = sum(1 for item in in_start_span if direct_decision(item) == 2)
            accepted_span = sum(1 for item in in_start_span if direct_decision(item) == 0)

            def decision_counts(items: list[tuple[dict[str, Any], dict[str, Any] | None, int, bool]]) -> dict[str, int]:
                counts: dict[str, int] = defaultdict(int)
                for item in items:
                    reason = direct_decision(item)
                    if reason is not None:
                        counts[decision_name(reason)] += 1
                return dict(sorted(counts.items()))

            first_raw = _ingress_reference(after_start[0][0]) if after_start else None
            per_card.append({
                "card": card,
                "startSendNs": card_start_ns,
                "startSendSucceeded": bool(card_starts) and not any(
                    int(item.get("length", 0) or 0) != 58 or int(item.get("error", 0) or 0) != 0
                    for item in card_starts
                ),
                "startSendAttemptCount": len(card_starts),
                "firstRawIngressAfterStartNs": first_raw.get("steadyNs") if first_raw else None,
                "firstRawIngressAfterStartLatencyNs": (
                    int(first_raw["steadyNs"]) - card_start_ns
                    if first_raw is not None and card_start_ns is not None else None
                ),
                "firstRawIngressAfterStart": first_raw,
                "firstDisabledIngress": first_with_reason(after_start, 2),
                "firstAcceptedIngress": first_with_reason(after_start, 0),
                "rawIngressAfterStartCount": raw_after_count,
                "matchedStage2DecisionCount": matched_after,
                "matchedDirectDecisionCount": direct_after,
                "unmatchedDecisionCount": missing_after,
                "disabledIngressCount": disabled_after,
                "acceptedIngressCount": accepted_after,
                "rawIngressInStartSpanCount": raw_span_count,
                "matchedStage2InStartSpanCount": matched_span,
                "matchedDisabledInStartSpanCount": disabled_span,
                "matchedAcceptedInStartSpanCount": accepted_span,
                "unmatchedInStartSpanCount": missing_span,
                "decisionCountsAfterStart": decision_counts(after_start),
                "decisionCountsInStartSpan": decision_counts(in_start_span),
            })
            session_raw_after_start += raw_after_count
            session_raw_in_span += raw_span_count
            session_matched += matched_after
            session_direct += direct_after
            session_missing += missing_after
            session_disabled += disabled_after
            session_accepted += accepted_after

        missing_cards = [card for card in range(expected_cards)
                         if card not in {int(item.get("card", -1)) for item in session_starts}]
        send_complete = bool(session_starts) and not missing_cards
        trace_incomplete = bool(application.get("traceIncomplete", True))
        has_gate_drop = any(int(card["matchedDisabledInStartSpanCount"]) > 0 for card in per_card)
        has_raw_and_direct = session_raw_after_start > 0 and session_direct > 0
        if not session_starts:
            classification = "start_trace_missing"
        elif failures:
            classification = "control_send_failed"
        elif not send_complete:
            classification = "start_trace_missing"
        elif has_gate_drop:
            classification = "application_start_gate_drop_observed"
        elif trace_incomplete:
            classification = "trace_incomplete"
        elif has_raw_and_direct:
            classification = "no_application_start_gate_drop_observed"
        else:
            classification = "unverifiable"
        classification_counts[classification] += 1
        sessions.append({
            "session": session,
            "classification": classification,
            "traceIncomplete": trace_incomplete,
            "expectedCardCount": expected_cards or None,
            "startSendCount": len(session_starts),
            "startSendSucceeded": send_complete and not failures,
            "startSendNs": start_ns,
            "lastStartSendNs": last_start_ns,
            "startSendSpanNs": last_start_ns - start_ns if start_ns is not None and last_start_ns is not None else None,
            "startSendSpanUs": ((last_start_ns - start_ns) / 1000.0
                                 if start_ns is not None and last_start_ns is not None else None),
            "cardSendOrder": send_order,
            "missingStartCards": missing_cards,
            "sendFailures": failures,
            "rawIngressCount": session_raw_after_start,
            "rawIngressInStartSpanCount": session_raw_in_span,
            "matchedStage2DecisionCount": session_matched,
            "matchedDirectDecisionCount": session_direct,
            "missingStage2JoinCount": session_missing,
            "disabledIngressCount": session_disabled,
            "acceptedIngressCount": session_accepted,
            "negativeConclusionDowngraded": trace_incomplete and not has_gate_drop,
            "perCard": per_card,
        })

    priority = (
        "application_start_gate_drop_observed", "control_send_failed",
        "start_trace_missing", "trace_incomplete",
        "no_application_start_gate_drop_observed", "unverifiable",
    )
    status = next((value for value in priority if classification_counts.get(value)), "start_trace_missing")
    return {
        "status": status,
        "traceIncomplete": bool(application.get("traceIncomplete", True)),
        "sessions": sessions,
        "summary": {
            "sessionCount": len(sessions),
            "sessionsWithStartTrace": sum(bool(by_session_starts.get(session)) for session in session_ids),
            "startSendCount": len(starts),
            "startSendFailureCount": sum(
                int(item.get("length", 0) or 0) != 58 or int(item.get("error", 0) or 0) != 0
                for item in starts
            ),
            "rawIngressCount": sum(item["rawIngressCount"] for item in sessions),
            "rawIngressInStartSpanCount": sum(item["rawIngressInStartSpanCount"] for item in sessions),
            "matchedStage2DecisionCount": sum(item["matchedStage2DecisionCount"] for item in sessions),
            "matchedDirectDecisionCount": sum(item["matchedDirectDecisionCount"] for item in sessions),
            "missingStage2JoinCount": sum(item["missingStage2JoinCount"] for item in sessions),
            "disabledIngressCount": sum(item["disabledIngressCount"] for item in sessions),
            "acceptedIngressCount": sum(item["acceptedIngressCount"] for item in sessions),
            "classificationCounts": dict(sorted(classification_counts.items())),
        },
        "limitations": [
            "Only stage 1 raw ingress with an exact session/card/correlation match to stage 2 can prove an application START-gate drop.",
            "Trigger and packet numbers are descriptive evidence and are never joined across measurement sessions.",
            "Missing stage 1 evidence does not prove that a card or FPGA did not transmit.",
            "When the application trace is incomplete, absence of Disabled decisions is classified as unknown or unverifiable.",
        ],
    }


def system_evidence(capture_dir: Path | None) -> dict[str, Any]:
    if capture_dir is None:
        return {"status": "not_provided", "classification": "unverifiable",
                "note": "no system-capture directory was supplied"}
    manifest_path = capture_dir / "system-capture-manifest.json"
    manifest = read_json(manifest_path)
    if not isinstance(manifest, dict):
        return {"status": "not_found", "classification": "unverifiable",
                "note": "system-capture-manifest.json is missing or unreadable",
                "directory": str(capture_dir)}
    pcap_path = capture_dir / "pktmon.pcapng"
    packets = []
    if pcap_path.exists():
        try:
            packets = _pcapng_packets(pcap_path)
        except (OSError, ValueError, struct.error) as exc:  # type: ignore[name-defined]
            packets = []
            manifest.setdefault("analysisErrors", []).append(str(exc))
    validation = read_json(capture_dir / "validation-summary.json", {}) or {}
    clock = read_json(capture_dir / "clock-anchors.json", {}) or {}
    status = manifest.get("status", "unknown")
    command_success = manifest.get("commandSuccess")
    artifact_valid = manifest.get("artifactValid")
    target_present = manifest.get("targetPacketsPresent")
    result = {
        "directory": str(capture_dir), "status": status,
        "classification": "verified" if status == "complete" and target_present is True
        else ("captured" if status in ("complete", "no_target_packets", "partial") else "unverifiable"),
        "trialId": manifest.get("trialId"), "captureSessionToken": manifest.get("captureSessionToken"),
        "runId": manifest.get("runId"), "listenId": manifest.get("listenId"),
        "commandSuccess": command_success, "artifactValid": artifact_valid,
        "targetPacketsPresent": target_present, "timeCoverage": manifest.get("timeCoverage"),
        "lossStatus": manifest.get("lossStatus"), "layerCoverage": manifest.get("layerCoverage"),
        "analysisReady": manifest.get("analysisReady"),
        "systemPackets": packets, "systemPacketCount": len(packets),
        "validation": validation, "clockAnchors": clock,
        "files": manifest.get("files", []), "manifest": manifest,
        "etlStopIo": [
            {key: command.get(key) for key in ("description", "exitCode", "startUtc", "endUtc", "durationMs", "stdout", "stderr")}
            for command in manifest.get("commands", [])
            if "stop" in str(command.get("description", "")).lower()
            or "convert" in str(command.get("description", "")).lower()
        ],
    }
    return result


def clock_evidence(application: dict[str, Any], system: dict[str, Any]) -> dict[str, Any]:
    metadata = application.get("metadata", {})
    app_start = metadata.get("monotonicAnchorNs")
    try:
        app_start = int(app_start)
    except (TypeError, ValueError):
        app_start = None
    system_clock = system.get("clockAnchors", {}) if isinstance(system, dict) else {}
    qpc_values = []
    for command in system.get("manifest", {}).get("commands", []) if isinstance(system, dict) else []:
        for key in ("startQpc", "endQpc"):
            value = command.get(key)
            if isinstance(value, (int, float)):
                qpc_values.append(int(value))
    return {
        "application": {
            "wallAnchorMs": metadata.get("wallAnchorMs"),
            "steadyAnchorNs": app_start,
            "steadyBoundsNs": [application.get("earliestSteadyNs"), application.get("latestSteadyNs")],
        },
        "system": {
            "utcNow": system_clock.get("utcNow"),
            "qpcNow": system_clock.get("qpcNow"),
            "qpcFrequency": system_clock.get("qpcFrequency"),
            "ready": system_clock.get("ready"),
        },
        "qpcBounds": [min(qpc_values), max(qpc_values)] if qpc_values else None,
        "status": "known" if app_start is not None and system_clock else "unknown",
        "note": "QPC/UTC anchors are evidence for alignment only; they do not prove packet causality.",
    }


def rounds_evidence(rounds: list[dict[str, Any]], application: dict[str, Any]) -> dict[str, Any]:
    events = []
    events_path = Path(application["root"]) / "events.jsonl"
    if events_path.exists():
        for line in events_path.read_text(encoding="utf-8", errors="replace").splitlines():
            value = read_json_line(line)
            if value is not None:
                events.append(value)
    stored_ids = set()
    for event in events:
        fields = event.get("fields", {}) if isinstance(event, dict) else {}
        candidate = fields.get("roundId") or event.get("roundId")
        if candidate is not None:
            stored_ids.add(str(candidate))
    expected = [str(item.get("roundId")) for item in rounds if item.get("roundId") is not None]
    missing = [item for item in expected if item not in stored_ids]
    return {
        "expectedRoundIds": expected, "storedRoundIds": sorted(stored_ids),
        "missingStoredRounds": missing,
        "storageLoss": bool(missing),
        "status": "storage_loss" if missing else ("complete" if expected else "not_specified"),
        "associationPolicy": {
            "crossRound16BitAssociation": False,
            "scope": "round/session/time-window only",
            "note": "16-bit trigger and packet numbers are never joined across rounds.",
        },
    }


def read_json_line(line: str) -> dict[str, Any] | None:
    try:
        value = json.loads(line)
    except ValueError:
        return None
    return value if isinstance(value, dict) else None


def evidence_matrix(application: dict[str, Any], system: dict[str, Any], clocks: dict[str, Any],
                    rounds: dict[str, Any], start: dict[str, Any] | None = None) -> list[dict[str, Any]]:
    start = start or {}
    start_status = start.get("status", "start_trace_missing")
    return [
        {"evidence": "application_per_packet_ingress", "available": bool(application.get("ingressPackets")),
         "verified": not application.get("traceIncomplete", True),
         "conclusion": "application ingress packet evidence available" if application.get("ingressPackets") else "no application ingress evidence"},
        {"evidence": "application_round_storage", "available": rounds.get("status") != "not_specified",
         "verified": not rounds.get("storageLoss", False),
         "conclusion": "missing stored rounds are storage loss" if rounds.get("storageLoss") else "no missing stored rounds observed"},
        {"evidence": "system_pktmon", "available": system.get("systemPacketCount", 0) > 0,
         "verified": system.get("classification") == "verified",
         "conclusion": "system-layer target packets present" if system.get("systemPacketCount", 0) > 0 else "system-layer packet presence unavailable"},
        {"evidence": "wpr_scheduling", "available": system.get("layerCoverage") in ("pktmon_plus_wpr",),
         "verified": system.get("layerCoverage") == "pktmon_plus_wpr",
         "conclusion": "WPR scheduling layer is present" if system.get("layerCoverage") == "pktmon_plus_wpr" else "WPR scheduling evidence absent, skipped, or unknown"},
        {"evidence": "clock_alignment", "available": clocks.get("status") == "known",
         "verified": clocks.get("status") == "known",
         "conclusion": "steady/QPC/UTC anchors are available" if clocks.get("status") == "known" else "clock alignment remains unknown"},
        {"evidence": "application_start_admission",
         "available": bool(application.get("startSends") or application.get("ingressPackets")),
         "verified": start_status in ("application_start_gate_drop_observed",
                                      "no_application_start_gate_drop_observed",
                                      "control_send_failed"),
         "conclusion": f"START admission evidence classification: {start_status}"},
    ]


def run(application_path: Path, system_path: Path | None, rounds_path: Path | None, output: Path) -> dict[str, Any]:
    temporary = None
    source_root = application_path
    if application_path.suffix.lower() == ".zip":
        temporary = tempfile.TemporaryDirectory(prefix="startup-diagnostics-")
        with zipfile.ZipFile(application_path) as archive:
            archive.extractall(temporary.name)
        source_root = Path(temporary.name)
    trace_root = _resolve_trace(source_root)
    metadata = read_json(trace_root / "run-config.json", {}) or {}
    expected = int(metadata.get("packetsPerTrig", 0) or 0)
    samples = metadata.get("samples")
    bits = int(metadata.get("bits", 32) or 32)
    if not expected and samples:
        expected = (int(samples) * (8 if bits == 32 else 4) + 1439) // 1440
    expected = max(1, expected)
    rounds_data = read_json(rounds_path, {}) if rounds_path else {}
    if isinstance(rounds_data, dict):
        rounds = rounds_data.get("rounds", []) or []
    elif isinstance(rounds_data, list):
        rounds = rounds_data
    else:
        rounds = []
    try:
        legacy = analyze_trace(trace_root, expected, int(metadata.get("dataPort", 18001)),
                               samples, bits, int(metadata.get("cards", 4) or 4), rounds, system_path)
    except Exception as exc:  # keep the startup evidence report inspectable
        legacy = {"error": f"reused parser failed: {exc}"}
    application = parse_application(trace_root)
    start = start_admission_evidence(application)
    system = system_evidence(system_path)
    clocks = clock_evidence(application, system)
    round_result = rounds_evidence(rounds, application)
    result = {
        "schemaVersion": 1,
        "analyzer": "startup_diagnostics_analyze.py",
        "applicationZip": str(application_path),
        "systemCapture": str(system_path) if system_path else None,
        "roundsJson": str(rounds_path) if rounds_path else None,
        "application": application,
        "startAdmissionEvidence": start,
        "systemCaptureEvidence": system,
        "rounds": round_result,
        "clockAnchors": clocks,
        "evidenceConclusionMatrix": evidence_matrix(application, system, clocks, round_result, start),
        "reusedTraceAnalyzer": legacy,
        "limitations": [
            "A missing system packet is not evidence of card/NIC/cable failure.",
            "A burst marker is a temporal marker, not capture success.",
            "Per-packet evidence is bounded by stored application records and artifact validity.",
        ],
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    if temporary is not None:
        temporary.cleanup()
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--application-zip", type=Path, required=True)
    parser.add_argument("--system-capture", type=Path)
    parser.add_argument("--rounds-json", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = run(args.application_zip, args.system_capture, args.rounds_json, args.output)
    print(json.dumps({
        "schemaVersion": result["schemaVersion"],
        "applicationTraceIncomplete": result["application"].get("traceIncomplete"),
        "applicationIngressPackets": len(result["application"].get("ingressPackets", [])),
        "systemStatus": result["systemCaptureEvidence"].get("status"),
        "systemClassification": result["systemCaptureEvidence"].get("classification"),
        "storageLoss": result["rounds"].get("storageLoss"),
        "startAdmissionStatus": result["startAdmissionEvidence"].get("status"),
        "startAdmissionSessions": len(result["startAdmissionEvidence"].get("sessions", [])),
    }, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
