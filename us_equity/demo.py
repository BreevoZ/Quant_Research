"""Deterministic synthetic Massive-shaped input. Never evidence of alpha."""

from pathlib import Path

import numpy as np
import pandas as pd

from .config import Experiment
from .data import calendar_for, minute_grid
from .pipeline import run, write_json


def generate_session(session: str, tickers: tuple[str, ...], seed: int = 7) -> pd.DataFrame:
    grid = minute_grid(calendar_for(session, session), session)
    rng = np.random.default_rng(seed)
    market = rng.normal(0, 0.0002, len(grid))
    frames = []
    for i, ticker in enumerate(tickers):
        returns = market + rng.normal(0, 0.0008, len(grid))
        close = (25 + i * 12) * np.exp(np.cumsum(returns))
        opening = np.r_[25 + i * 12, close[:-1]]
        frames.append(pd.DataFrame({"ticker": ticker, "window_start": grid.asi8,
                                   "open": opening, "high": np.maximum(opening, close) * 1.0002,
                                   "low": np.minimum(opening, close) * 0.9998, "close": close,
                                   "volume": rng.integers(100, 20_000, len(grid)),
                                   "transactions": rng.integers(1, 100, len(grid))}))
    return pd.concat(frames, ignore_index=True)


def demo(output: Path) -> list[Path]:
    output.mkdir(parents=True, exist_ok=False)
    raw = output / "raw"
    raw.mkdir()
    tickers = tuple(f"SYN{i:02d}" for i in range(12))
    # DST transition plus Thanksgiving half-day. All three days are historical.
    dates = ["2025-03-07", "2025-03-10", "2025-11-28"]
    write_json(raw / "SYNTHETIC.json", {"synthetic": True, "seed": 7, "sessions": dates})
    for i, session in enumerate(dates):
        data = generate_session(session, tickers, seed=7 + i)
        data = data.drop(index=100)  # One missing minute; never fill it.
        premarket = data.iloc[[0]].copy()
        premarket["window_start"] -= 60_000_000_000
        data = pd.concat([premarket, data], ignore_index=True)
        data.to_csv(raw / f"{session}.csv", index=False)
    reports = []
    for session in dates:
        experiment = Experiment(start=session, end=session, tickers=tickers)
        reports.append(run(experiment, raw, output / session, synthetic=True))
    (output / "README.md").write_text(
        "# Synthetic US equity demo\n\nThese reports test the pipeline, not a trading strategy.\n\n"
        + "\n".join(f"- [{day}]({day}/report.md)" for day in dates) + "\n", encoding="utf-8")
    return reports
