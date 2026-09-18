"""Analyze the trace produced by the real paimage_start_race test."""
from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path


def fail(message: str) -> None:
    raise AssertionError(message)


def load_analyzer(path: Path):
    spec = importlib.util.spec_from_file_location("startup_diagnostics_analyze", path)
    if not spec or not spec.loader:
        fail(f"cannot load analyzer: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: paimage_start_race_analyzer_test.py ARTIFACT_ROOT ANALYZER")
    artifact_root = Path(sys.argv[1])
    analyzer_path = Path(sys.argv[2])
    runs = sorted((path for path in artifact_root.glob("run-*") if path.is_dir()),
                  key=lambda path: path.stat().st_mtime_ns)
    if not runs:
        fail(f"no paimage_start_race artifact under {artifact_root}")
    root = runs[-1]
    result_path = root / "result.json"
    race_result = json.loads(result_path.read_text(encoding="utf-8"))
    if not race_result.get("passed"):
        fail(f"race result is not passed: {result_path}")

    analyzer = load_analyzer(analyzer_path)
    output = root / "startup-diagnostics-analysis.json"
    result = analyzer.run(root, None, None, output)
    evidence = result["startAdmissionEvidence"]
    sessions = evidence["sessions"]
    normal = [row for row in sessions if 1000 <= int(row["session"]) < 1060]
    failed = next((row for row in sessions if int(row["session"]) == 9000), None)
    recovery = next((row for row in sessions if int(row["session"]) == 9001), None)

    if len(normal) != 60:
        fail(f"expected 60 normal sessions, got {len(normal)}")
    if any(row["classification"] != "application_start_fence_clean" for row in normal):
        fail("a normal race session was not classified clean")
    if any(row["missingStage2JoinCount"] != 0 or row["disabledIngressCount"] != 0
           or row["traceIncomplete"] for row in normal):
        fail("a normal race session has missing, disabled, or incomplete evidence")
    if failed is None or failed["classification"] != "application_start_fence_failed":
        fail("injected START failure was not classified application_start_fence_failed")
    if recovery is None or recovery["classification"] != "application_start_fence_clean":
        fail("recovery session was not classified clean")

    summary = evidence["summary"]
    counts = summary["classificationCounts"]
    if counts.get("application_start_fence_clean") != 61:
        fail(f"unexpected clean classification count: {counts}")
    if counts.get("application_start_fence_inconclusive", 0) != 0:
        fail(f"inconclusive race session observed: {counts}")
    if counts.get("application_start_gate_drop_observed", 0) != 0:
        fail(f"gate-drop race session observed: {counts}")
    if summary["missingStage2JoinCount"] != 0:
        fail(f"missing stage2 joins: {summary['missingStage2JoinCount']}")
    if result["startAdmissionEvidence"]["traceIncomplete"] is not False:
        fail("real race trace was incomplete")

    race_result["startAdmissionAnalyzer"] = {
        "successSessionCount": len(normal),
        "cleanSessionCount": counts.get("application_start_fence_clean", 0),
        "failedSessionCount": counts.get("application_start_fence_failed", 0),
        "inconclusiveSuccessSessions": counts.get("application_start_fence_inconclusive", 0),
        "gateDropSuccessSessions": counts.get("application_start_gate_drop_observed", 0),
        "missingStage2JoinCount": summary["missingStage2JoinCount"],
        "rawIngressCount": summary["rawIngressCount"],
        "heldIngressCount": summary["heldIngressCount"],
        "releasedIngressCount": summary["releasedIngressCount"],
        "traceIncomplete": evidence["traceIncomplete"],
    }
    result_path.write_text(json.dumps(race_result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print(
        "realTraceAnalyzer=passed "
        f"successSessionCount={len(normal)} "
        f"cleanSessionCount={counts.get('application_start_fence_clean', 0)} "
        f"failedSessionCount={counts.get('application_start_fence_failed', 0)} "
        f"inconclusiveSuccessSessions={counts.get('application_start_fence_inconclusive', 0)} "
        f"gateDropSuccessSessions={counts.get('application_start_gate_drop_observed', 0)} "
        f"missingStage2JoinCount={summary['missingStage2JoinCount']} "
        f"rawIngressCount={summary['rawIngressCount']} "
        f"heldIngressCount={summary['heldIngressCount']} "
        f"releasedIngressCount={summary['releasedIngressCount']}"
    )


if __name__ == "__main__":
    main()
