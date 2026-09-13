"""Delayed open-to-open return proxies, not simulated executable fills."""

import pandas as pd


def compute_labels(bars: pd.DataFrame, horizons: tuple[int, ...], skip_minutes: int) -> pd.DataFrame:
    if skip_minutes < 1:
        raise ValueError("Leave at least one whole minute after the signal bar closes")
    # Row t describes [t,t+1). Signal is available at t+1; skip=1 enters at t+2.
    entry_offset = 1 + skip_minutes
    price = bars.open.where(bars.volume > 0)
    result = pd.DataFrame(index=bars.index)
    result["entry_proxy_open"] = price.shift(-entry_offset)
    signal_observed = bars.close.notna() & bars.volume.gt(0)
    for horizon in horizons:
        exit_offset = entry_offset + horizon
        complete = price.rolling(horizon + 1).count().shift(-exit_offset).eq(horizon + 1)
        result[f"fwd_{horizon}m"] = (
            price.shift(-exit_offset) / result.entry_proxy_open - 1
        ).where(complete & signal_observed)
    return result
