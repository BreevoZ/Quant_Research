"""Daily bounded-memory pipeline with source fingerprints and run manifests."""

from datetime import datetime, timezone
import hashlib
from importlib.metadata import version
import json
from pathlib import Path
import platform

import pandas as pd

from . import __version__
from .config import Experiment
from .data import calendar_for, discover_files, load_session, minute_grid
from .evaluate import evaluate_session, render_report, summarize
from .features import compute_features
from .labels import compute_labels


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")


def build_panel(bars: pd.DataFrame, grid: pd.DatetimeIndex, session: str,
                experiment: Experiment) -> pd.DataFrame:
    parts = []
    indexed = {ticker: group.set_index("ts").drop(columns="ticker")
               for ticker, group in bars.groupby("ticker")}
    for ticker in experiment.tickers:
        source = indexed.get(ticker, pd.DataFrame(columns=["open", "high", "low", "close", "volume"], dtype=float))
        full = source.reindex(grid)
        features = compute_features(full, experiment.lookbacks)
        labels = compute_labels(full, experiment.horizons, experiment.skip_minutes)
        panel = pd.concat([full, features, labels], axis=1)
        panel.index.name = "ts"
        panel["ticker"], panel["session"] = ticker, session
        panel["minute"] = range(len(grid))
        panel["available_at"] = grid + pd.Timedelta(minutes=1)
        panel["observed"] = full.close.notna() & full.volume.gt(0)
        panel["price_basis"] = "raw" if experiment.source == "flatfiles" else "split_adjusted"
        parts.append(panel.reset_index())
    return pd.concat(parts, ignore_index=True).sort_values(["ts", "ticker"]).reset_index(drop=True)


def run(experiment: Experiment, input_root: Path, output: Path, *, synthetic: bool = False) -> Path:
    synthetic = synthetic or (input_root / "SYNTHETIC.json").is_file()
    calendar = calendar_for(experiment.start, experiment.end)
    sessions = calendar.sessions_in_range(experiment.start, experiment.end).strftime("%Y-%m-%d").tolist()
    if not sessions:
        raise ValueError("No XNYS trading sessions in the selected date range")
    files = discover_files(input_root, experiment)
    missing = [day for day in sessions if day not in files]
    if missing:
        raise FileNotFoundError(f"Missing session files under {input_root}: {', '.join(missing)}")
    # Fresh run directories prevent stale panels or accidental overwrites.
    output.mkdir(parents=True, exist_ok=False)
    (output / "panels").mkdir()
    manifest = {
        "status": "running", "synthetic": synthetic, "version": __version__,
        "created_at_utc": datetime.now(timezone.utc).isoformat(),
        "experiment": experiment.to_dict(), "input_root": str(input_root.resolve()),
        "source_contract": {"timestamp_unit": "ns" if experiment.source == "flatfiles" else "ms",
                            "price_basis": "raw" if experiment.source == "flatfiles" else "split_adjusted",
                            "calendar": "XNYS", "calendar_scope": "regular US cash equity session",
                            "universe": "explicit_fixed_list_not_point_in_time"},
        "labels": "signal bar [t,t+1); entry open[t+1+skip]; exit open[t+1+skip+h]; same session",
        "python": platform.python_version(),
        "dependencies": {name: version(name) for name in ("numpy", "pandas", "pyarrow", "exchange_calendars")},
        "code_sha256": {p.name: sha256(p) for p in sorted(Path(__file__).parent.glob("*.py"))},
        "inputs": [],
    }
    write_json(output / "manifest.json", manifest)
    qa, buckets = [], []
    try:
        for session in sessions:
            for path in files[session]:
                manifest["inputs"].append({"path": str(path.resolve()), "size": path.stat().st_size,
                                           "sha256": sha256(path)})
            grid = minute_grid(calendar, session)
            bars, quality = load_session(files[session], experiment, session, grid)
            panel = build_panel(bars, grid, session, experiment)
            panel.to_parquet(output / "panels" / f"{session}.parquet", index=False)
            buckets.append(evaluate_session(panel, experiment))
            qa.append(quality)
            print(f"{session}: {len(bars):,} bars, {sum(quality['missing_minutes'].values()):,} missing minutes")
        bucket_table = pd.concat(buckets, ignore_index=True)
        daily, summary = summarize(bucket_table)
        bucket_table.to_csv(output / "bucket_ic.csv", index=False)
        daily.to_csv(output / "daily_ic.csv", index=False)
        summary.to_csv(output / "summary.csv", index=False)
        write_json(output / "qa.json", qa)
        (output / "report.md").write_text(render_report(summary, qa, synthetic), encoding="utf-8")
        manifest["status"] = "complete"
        manifest["sessions"] = sessions
        manifest["diagnostic_rows"] = len(summary)
        manifest["completed_at_utc"] = datetime.now(timezone.utc).isoformat()
        write_json(output / "manifest.json", manifest)
    except Exception as exc:
        manifest["status"] = "failed"
        manifest["error"] = f"{type(exc).__name__}: {exc}"
        write_json(output / "manifest.json", manifest)
        raise
    return output / "report.md"
