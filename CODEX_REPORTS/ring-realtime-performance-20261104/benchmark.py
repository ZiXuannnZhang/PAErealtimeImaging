"""Serialized synthetic FBP A/B benchmark. Does not access acquisition hardware.

Examples (run from any directory):
  python benchmark.py --quick
  python benchmark.py --full --out artifacts/fbp-performance-work/performance/full
  python benchmark.py --cpu --candidate-bin artifacts/build-delivery/<candidate>/bin

One new selftest drives both the accepted baseline service and the candidate.
Run alone: the production service/selftest uses fixed local port 5555 and SHM.
This is a serialized compute budget measurement, not a paced 40 Hz ingress test.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timedelta, timezone
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BIN = ROOT / "MC_410T_MultiCard/delivery/build/mingw_debug/bin"
BASELINE_BIN = ROOT / "artifacts/build-delivery/20261104-133957_21d60c0a_ring_cleanup_local/bin"
TZ = timezone(timedelta(hours=8))
NUMBER = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"
TIMING = re.compile(rf"\[block-timing\] submit=(\d+) roundtrip_ms=({NUMBER}) service_ms=({NUMBER})")
STAGES = re.compile(rf"\[fbp-stages\] backend=(\w+) preprocess_ms=({NUMBER}) accumulate_ms=({NUMBER})")
PREPARE = re.compile(rf"\[fbp-metrics\] prepare_us=({NUMBER})")
BLOCKS = 21
BUDGET_MS = 250.0


def timestamp() -> str:
    return datetime.now(TZ).isoformat(timespec="milliseconds")


def absolute(path: str | Path) -> Path:
    p = Path(path)
    return (p if p.is_absolute() else ROOT / p).resolve()


def write_json(path: Path, value) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def file_receipt(path: Path) -> dict:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(chunk)
    stat = path.stat()
    return {"path": str(path), "bytes": stat.st_size, "mtime_ns": stat.st_mtime_ns,
            "sha256": digest.hexdigest()}


def statistics_ms(values: list[float]) -> dict:
    if not values or not all(math.isfinite(v) and v >= 0 for v in values):
        raise ValueError("Missing/nonfinite/negative timing")
    ordered = sorted(values)
    return {"count": len(values), "mean": statistics.mean(values),
            "median": statistics.median(values), "p95": ordered[math.ceil(.95 * len(values)) - 1],
            "min": ordered[0], "max": ordered[-1],
            "over_250ms": sum(v > BUDGET_MS for v in values)}


def parse_log(text: str, expected_backend: str | None) -> dict:
    samples, pending_stage, preparation = [], None, []
    for line in text.splitlines():
        if match := PREPARE.search(line):
            preparation.append(float(match[1]) / 1000.0)
        if match := STAGES.search(line):
            if pending_stage is not None:
                raise ValueError("Unpaired duplicate stage record")
            pending_stage = {"backend": match[1], "preprocess_ms": float(match[2]),
                             "accumulate_ms": float(match[3])}
        if match := TIMING.search(line):
            sample = {"submit": int(match[1]), "roundtrip_ms": float(match[2]),
                      "service_ms": float(match[3])}
            if pending_stage is not None:
                sample.update(pending_stage)
            samples.append(sample)
            pending_stage = None
    if pending_stage is not None or len(samples) != BLOCKS:
        raise ValueError(f"Expected {BLOCKS} complete timings, received {len(samples)}")
    if [s["submit"] for s in samples] != list(range(1, BLOCKS + 1)):
        raise ValueError("Unexpected submit sequence")
    if f"[benchmark] normal stop after {BLOCKS} complete blocks" not in text:
        raise ValueError("Missing normal benchmark completion")
    if not re.search(rf"done: channels=\d+ K=\d+ rounds=\d+ blocks={BLOCKS}\b", text):
        raise ValueError("Missing complete selftest summary")
    if expected_backend is not None:
        if any(s.get("backend") != expected_backend for s in samples):
            raise ValueError(f"Candidate did not report required backend {expected_backend!r}")
    observation = re.findall(r"\[RingSHMObs\] service kind=final[^\r\n]*", text)
    if not observation:
        raise ValueError("Missing final service observation")
    counters = {key: int(value) for key, value in re.findall(
        r"\b(notifications|consumed|mismatch|ready_zero|duplicate|gap)=(\d+)", observation[-1])}
    if counters.get("consumed") != BLOCKS or any(counters.get(k) != 0 for k in ("mismatch", "ready_zero", "duplicate", "gap")):
        raise ValueError(f"Unexpected serialized service counters: {counters}")
    warm = samples[1:]
    metrics = {name: statistics_ms([s[name] for s in warm]) for name in ("roundtrip_ms", "service_ms")}
    if all("preprocess_ms" in s for s in warm):
        for name in ("preprocess_ms", "accumulate_ms"):
            metrics[name] = statistics_ms([s[name] for s in warm])
    return {"cold": samples[0], "samples": samples, "warm_count": len(warm),
            "warm": metrics, "prepare_ms_reported": preparation, "service_counters": counters}


class WindowsJob:
    """Kill-on-close job containing only the selftest we spawn and its children."""
    def __init__(self):
        if os.name != "nt":
            raise RuntimeError("Execution requires Windows; --dry-run is platform independent")
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)

        class Basic(ctypes.Structure):
            _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                        ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                        ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                        ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD),
                        ("SchedulingClass", wintypes.DWORD)]

        class Io(ctypes.Structure):
            _fields_ = [(name, ctypes.c_uint64) for name in
                        ("ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                         "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

        class Extended(ctypes.Structure):
            _fields_ = [("BasicLimitInformation", Basic), ("IoInfo", Io),
                        ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                        ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]

        self.kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        self.kernel.CreateJobObjectW.restype = wintypes.HANDLE
        self.kernel.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        self.kernel.SetInformationJobObject.restype = wintypes.BOOL
        self.kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        self.kernel.AssignProcessToJobObject.restype = wintypes.BOOL
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel.CloseHandle.restype = wintypes.BOOL
        self.handle = self.kernel.CreateJobObjectW(None, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        settings = Extended()
        settings.BasicLimitInformation.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.kernel.SetInformationJobObject(self.handle, 9, ctypes.byref(settings), ctypes.sizeof(settings)):
            error = ctypes.WinError(ctypes.get_last_error())
            self.close()
            raise error

    def attach(self, process: subprocess.Popen) -> None:
        # Popen owns this exact process handle; it cannot identify an unrelated reused PID.
        if not self.kernel.AssignProcessToJobObject(self.handle, wintypes.HANDLE(int(process._handle))):
            raise ctypes.WinError(ctypes.get_last_error())

    def close(self) -> None:
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def run_process(command: list[str], cwd: Path, log_path: Path, timeout: float) -> tuple[int, bool]:
    env = os.environ.copy()
    env["QT_FORCE_STDERR_LOGGING"] = "1"
    process = None
    job = WindowsJob()
    timed_out = False
    try:
        with log_path.open("wb") as log:
            process = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                       stdout=log, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                job.attach(process)
            except Exception:
                # Assignment failed before the normal test proceeds. Stop only this tree.
                subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                               stdout=log, stderr=subprocess.STDOUT, check=False,
                               creationflags=subprocess.CREATE_NO_WINDOW)
                process.wait(timeout=10)
                raise
            try:
                process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                job.close()  # Exact job membership, never a process-name kill.
                process.wait(timeout=10)
            finally:
                job.close()  # Also removes a selftest child left behind by std::exit.
        return process.returncode, timed_out
    finally:
        job.close()
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=10)


def make_plan(args) -> list[dict]:
    mode = "cpu" if args.cpu else "full" if args.full else "quick"
    cases = [(2, "on", .375)] if mode == "quick" else list(itertools.product((2,), ("off", "on"), (0., .375)))
    if mode == "cpu":
        cases = [(2, "on", .375)]
    plan = []
    for index, (channels, timevar, shift) in enumerate(cases):
        order = ("candidate_cpu",) if mode == "cpu" else (("baseline", "candidate") if index % 2 == 0 else ("candidate", "baseline"))
        for build in order:
            candidate = build != "baseline"
            backend = "cpu" if build in ("baseline", "candidate_cpu") else "auto"
            service_bin = args.candidate_bin if candidate else args.baseline_bin
            name = f"c{channels}_tv{timevar}_shift{str(shift).replace('.', 'p')}_{build}"
            command = [str(args.selftest_bin / "ring_svc_selftest.exe"), "--svc", str(service_bin / "ImagingSvc.exe"),
                       "--synthetic", "1", "--id", "14", "--exact-fbp", "1", "--no-save", "1",
                       "--rounds", "2", "--frame-timeout-ms", "120000", "--benchmark-blocks", str(BLOCKS),
                       "--samples", "7500", "--block", "10", "--grid-mm", "0.01", "--channels", str((1 << channels) - 1),
                       "--radius-per-ch", ",".join(["6.48"] * 2 + ["6.57"] * 6),
                       "--sys-delay-per-ch", ",".join(["448", "464"] * 2 + ["358", "371"] * 6),
                       "--splice", "1", "--splice-blend", "5", "--mask-length", "375",
                       "--sos", "1490,1540", "--sos-radii-mm", "7.5", "--timevar-lowpass", timevar,
                       "--timevar-backend", backend, "--fbp-shift", str(shift), "--out", str(args.out / "selftest-output" / name)]
            plan.append({"name": name, "build": build, "channels": channels, "timevar": timevar,
                         "shift_samples": shift, "backend_requested": backend,
                         "backend_expected": ("off" if timevar == "off" else "cpu" if build == "candidate_cpu" else "cuda") if candidate else None,
                         "command": command, "cwd": str(args.selftest_bin), "service_bin": str(service_bin)})
    return plan


def write_summary(out: Path, records: list[dict], mode: str) -> None:
    write_json(out / "results.json", {"schema_version": 1, "mode": mode, "updated_at": timestamp(),
                                     "budget_ms": BUDGET_MS, "sampling": "serialized; first block excluded; no warm samples removed",
                                     "runs": records})
    lines = ["# FBP synthetic performance measurements", "",
             "Serialized submission; first block excluded. P95 is nearest-rank. All warm samples retained.",
             "250 ms is the compute budget for 10 triggers at a combined 40 Hz. This is not sustained-ingress acceptance.", "",
             "|Build|Channels|STFT|Shift|Warm n|Service mean / median / P95 / max ms|Roundtrip mean / median / P95 / max ms|Roundtrip >250 ms|Preprocess mean ms|Accumulate mean ms|",
             "|---|---:|---|---:|---:|---|---|---:|---:|---:|"]
    for row in records:
        if row.get("status") != "passed":
            lines.append(f"\nStopped at `{row['name']}`: {row.get('error', row.get('status'))}. See its complete log/receipt.")
            continue
        warm = row["measurement"]["warm"]
        def format_stats(name):
            return " / ".join(f"{warm[name][key]:.3f}" for key in ("mean", "median", "p95", "max"))
        def stage(name):
            return f"{warm[name]['mean']:.3f}" if name in warm else "—"
        lines.append(f"|{row['build']}|{row['channels']}|{row['timevar']}|{row['shift_samples']}|{row['measurement']['warm_count']}|"
                     f"{format_stats('service_ms')}|{format_stats('roundtrip_ms')}|{warm['roundtrip_ms']['over_250ms']}|"
                     f"{stage('preprocess_ms')}|{stage('accumulate_ms')}|")
    (out / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--quick", action="store_true", help="2 channels, STFT on, .375 shift; baseline + candidate (default)")
    group.add_argument("--full", action="store_true", help="4 two-channel workload combinations, alternating baseline/candidate order")
    group.add_argument("--cpu", action="store_true", help="candidate CPU STFT override, 2 channels, .375 shift")
    parser.add_argument("--candidate-bin", type=absolute, default=DEFAULT_BIN)
    parser.add_argument("--baseline-bin", type=absolute, default=BASELINE_BIN)
    parser.add_argument("--selftest-bin", type=absolute, default=None, help="new harness binary directory; defaults to candidate-bin")
    parser.add_argument("--out", type=absolute, default=None, help="new/empty output directory; existing evidence is never overwritten")
    parser.add_argument("--timeout-seconds", type=float, default=600, help="whole-run timeout, including preparation")
    parser.add_argument("--dry-run", action="store_true", help="print commands only, do not launch any process or create evidence")
    args = parser.parse_args()
    mode = "cpu" if args.cpu else "full" if args.full else "quick"
    args.selftest_bin = args.selftest_bin or args.candidate_bin
    args.out = args.out or ROOT / "artifacts/fbp-performance-work/performance" / (datetime.now(TZ).strftime("%Y%m%d-%H%M%S") + "_" + mode)
    if not math.isfinite(args.timeout_seconds) or args.timeout_seconds < 1:
        parser.error("--timeout-seconds must be finite and positive")
    plan = make_plan(args)
    if args.dry_run:
        print(json.dumps(plan, ensure_ascii=False, indent=2))
        return 0
    if args.out.exists() and any(args.out.iterdir()):
        parser.error(f"Output directory is not empty: {args.out}")
    paths = {args.selftest_bin / "ring_svc_selftest.exe"}
    bins = {Path(row["service_bin"]) for row in plan} | {args.selftest_bin}
    for directory in bins:
        for name in ("ImagingSvc.exe", "ring_recon_cuda.dll", "cudart64_12.dll", "cufft64_11.dll",
                     "cufft64_12.dll", "cublas64_12.dll", "cublasLt64_12.dll"):
            path = directory / name
            if path.is_file():
                paths.add(path)
    for row in plan:
        if not Path(row["command"][2]).is_file():
            parser.error(f"Service missing: {row['command'][2]}")
    if not (args.selftest_bin / "ring_svc_selftest.exe").is_file():
        parser.error("New ring_svc_selftest.exe is missing")
    args.out.mkdir(parents=True, exist_ok=True)
    receipts = [file_receipt(path) for path in sorted(paths)]
    def git(*arguments):
        run = subprocess.run(["git", *arguments], cwd=ROOT, capture_output=True, text=True)
        return run.stdout.strip() if run.returncode == 0 else None
    write_json(args.out / "provenance.json", {"created_at": timestamp(), "repository": str(ROOT), "source_sha": git("rev-parse", "HEAD"),
                "branch": git("branch", "--show-current"), "tracked_changes": git("status", "--porcelain", "--untracked-files=no"),
                "binary_receipts": receipts, "harness_source": file_receipt(Path(__file__).resolve()), "plan": plan,
                "environment_overrides": {"QT_FORCE_STDERR_LOGGING": "1"},
                "process_cleanup": "Windows kill-on-close Job Object scoped to each launched selftest and descendants"})
    records = []
    for planned in plan:
        row = dict(planned)
        row.update(started_at=timestamp(), status="running", log=str(args.out / (row["name"] + ".log")))
        records.append(row)
        write_json(args.out / (row["name"] + ".json"), row)
        write_summary(args.out, records, mode)
        print("START", row["name"], flush=True)
        begin = time.monotonic()
        try:
            for receipt in receipts:
                stat = Path(receipt["path"]).stat()
                if stat.st_size != receipt["bytes"] or stat.st_mtime_ns != receipt["mtime_ns"]:
                    raise RuntimeError(f"Binary changed during experiment: {receipt['path']}")
            code, timed_out = run_process(row["command"], Path(row["cwd"]), Path(row["log"]), args.timeout_seconds)
            row.update(returncode=code, timed_out=timed_out)
            if timed_out or code != 0:
                raise RuntimeError(f"Selftest {'timed out' if timed_out else 'failed'}; exit={code}")
            row["measurement"] = parse_log(Path(row["log"]).read_text(encoding="utf-8", errors="replace"), row["backend_expected"])
            row["status"] = "passed"
        except Exception as error:
            row.update(status="failed", error=str(error))
        row.update(finished_at=timestamp(), wall_seconds=time.monotonic() - begin)
        write_json(args.out / (row["name"] + ".json"), row)
        write_summary(args.out, records, mode)
        print("DONE", row["name"], row["status"], f"{row['wall_seconds']:.2f}s", flush=True)
        if row["status"] != "passed":
            print(row["error"], file=sys.stderr)
            return 1
    print("Evidence:", args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
