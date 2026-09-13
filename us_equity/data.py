"""Read one session at a time; never infer timestamp units or adjust prices."""

from collections import defaultdict
from pathlib import Path
import re

import exchange_calendars as xcals
import numpy as np
import pandas as pd

from .config import Experiment

REQUIRED = ["ticker", "window_start", "open", "high", "low", "close", "volume"]
REST_NAMES = {"t": "window_start", "o": "open", "h": "high", "l": "low",
              "c": "close", "v": "volume"}


def calendar_for(start: str, end: str):
    return xcals.get_calendar(
        "XNYS", start=pd.Timestamp(start) - pd.Timedelta(days=7),
        end=pd.Timestamp(end) + pd.Timedelta(days=7),
    )


def minute_grid(calendar, session: str) -> pd.DatetimeIndex:
    """Bar START times: include open, exclude close, including on half days."""
    return pd.date_range(calendar.session_open(session), calendar.session_close(session),
                         freq="min", inclusive="left").as_unit("ns")


def discover_files(root: Path, experiment: Experiment) -> dict[str, list[Path]]:
    if not root.is_dir():
        raise FileNotFoundError(f"Minute input directory does not exist: {root}")
    files = defaultdict(list)
    for path in sorted(root.rglob("*.csv*")):
        if not path.is_file() or not (path.name.endswith(".csv") or path.name.endswith(".csv.gz")):
            continue
        match = re.match(r"^(\d{4}-\d{2}-\d{2})(?:_|\.csv)", path.name)
        if match and experiment.start <= match[1] <= experiment.end:
            files[match[1]].append(path)
    return dict(files)


def normalize_bars(raw: pd.DataFrame, source: str, session: str) -> pd.DataFrame:
    if source not in {"flatfiles", "rest_split_adjusted"}:
        raise ValueError(f"Unknown source profile: {source}")
    frame = raw.copy()
    if source == "rest_split_adjusted" and "t" in frame:
        if "window_start" in frame:
            raise ValueError("Mixed REST schemas in one file")
        frame = frame.rename(columns=REST_NAMES)
    missing = set(REQUIRED) - set(frame)
    if missing:
        raise ValueError(f"Missing minute columns: {sorted(missing)}")
    frame = frame[REQUIRED].copy()
    if frame.empty:
        return pd.DataFrame(columns=["ticker", "ts", "open", "high", "low", "close", "volume"])
    if frame.ticker.isna().any() or frame.ticker.str.strip().eq("").any():
        raise ValueError("Empty ticker")
    values = frame[["open", "high", "low", "close", "volume"]].apply(pd.to_numeric, errors="raise")
    if not np.isfinite(values.to_numpy()).all():
        raise ValueError("Non-finite OHLCV")
    if (values[["open", "high", "low", "close"]] <= 0).any().any() or (values.volume < 0).any():
        raise ValueError("Prices must be positive and volume nonnegative")
    if ((values.high < values[["open", "close", "low"]].max(axis=1))
            | (values.low > values[["open", "close", "high"]].min(axis=1))).any():
        raise ValueError("Invalid OHLC bounds")
    frame[values.columns] = values
    # Explicit profile prevents the downloader's ms REST values being read as ns.
    unit = "ns" if source == "flatfiles" else "ms"
    stamps = pd.to_numeric(frame.pop("window_start"), errors="raise")
    if not pd.api.types.is_integer_dtype(stamps.dtype):
        raise ValueError("window_start must be integer epoch timestamps")
    # Bound integers before datetime conversion: pandas 3 can represent ms dates
    # beyond Python's year range, so errors='coerce' alone does not detect ns-as-ms.
    midnight = pd.Timestamp(session, tz="America/New_York")
    scale = 1 if unit == "ns" else 1_000_000
    lower, upper = midnight.value // scale, (midnight + pd.DateOffset(days=1)).value // scale
    if not stamps.between(lower, upper, inclusive="left").all():
        raise ValueError(f"Timestamp unit/date mismatch for {source}, session {session}")
    frame["ts"] = pd.to_datetime(stamps, unit=unit, utc=True, errors="coerce")
    if (frame.ts.isna().any() or not frame.ts.eq(frame.ts.dt.floor("min")).all()
            or not frame.ts.dt.tz_convert("America/New_York").dt.strftime("%Y-%m-%d").eq(session).all()):
        raise ValueError(f"Timestamp unit/date mismatch for {source}, session {session}")
    frame["ts"] = frame.ts.dt.as_unit("ns")
    return frame


def load_session(files: list[Path], experiment: Experiment, session: str,
                 grid: pd.DatetimeIndex) -> tuple[pd.DataFrame, dict]:
    chunks, source_rows, selected_rows = [], 0, 0
    for path in files:
        for raw in pd.read_csv(path, chunksize=250_000, dtype={"ticker": "str"}):
            if "ticker" not in raw:
                raise ValueError(f"Missing ticker column: {path}")
            source_rows += len(raw)
            raw = raw[raw.ticker.isin(experiment.tickers)]
            selected_rows += len(raw)
            if not raw.empty:
                chunks.append(normalize_bars(raw, experiment.source, session))
    if not chunks:
        raise ValueError(f"No selected tickers found on {session}")
    bars = pd.concat(chunks, ignore_index=True)
    if bars.duplicated(["ticker", "ts"]).any():
        raise ValueError(f"Duplicate ticker/minute on {session}; do not mix files or sources")
    regular = bars.ts.isin(grid)
    qa = {"session": session, "source_rows": source_rows, "selected_rows": selected_rows,
          "outside_regular_rows": int((~regular).sum()), "regular_rows": int(regular.sum()),
          "zero_volume_rows": int((bars.loc[regular, "volume"] == 0).sum()),
          "expected_minutes_per_ticker": len(grid), "requested_tickers": len(experiment.tickers)}
    bars = bars.loc[regular].sort_values(["ticker", "ts"]).reset_index(drop=True)
    if bars.empty:
        raise ValueError(f"No regular-session bars on {session}")
    counts = bars.groupby("ticker").size()
    qa["missing_minutes"] = {t: len(grid) - int(counts.get(t, 0)) for t in experiment.tickers}
    return bars, qa
