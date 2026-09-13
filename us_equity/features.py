"""Intraday-only features on a complete clock grid, with no forward filling."""

import numpy as np
import pandas as pd


def feature_names(lookbacks: tuple[int, ...]) -> list[str]:
    return [f"ret_{w}m" for w in lookbacks] + ["rv_30m", "volume_ratio_30m", "range_pct"]


def compute_features(bars: pd.DataFrame, lookbacks: tuple[int, ...]) -> pd.DataFrame:
    # A zero-volume or absent bar is not a usable price observation.
    close = bars.close.where(bars.volume > 0)
    volume = bars.volume.where(bars.volume > 0)
    result = pd.DataFrame(index=bars.index)
    for window in lookbacks:
        continuous = close.rolling(window + 1).count().eq(window + 1)
        result[f"ret_{window}m"] = (close / close.shift(window) - 1).where(continuous)
    log_return = np.log(close / close.shift(1))
    result["rv_30m"] = np.sqrt(log_return.pow(2).rolling(30, min_periods=30).sum())
    baseline = volume.shift(1).rolling(30, min_periods=30).mean()
    result["volume_ratio_30m"] = volume / baseline
    result["range_pct"] = (bars.high - bars.low) / close
    return result.replace([np.inf, -np.inf], np.nan)
