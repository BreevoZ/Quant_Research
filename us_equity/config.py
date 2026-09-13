"""Small, explicit experiment contract. Paths are resolved by qr.paths."""

from dataclasses import asdict, dataclass, fields
from datetime import date
from pathlib import Path
import tomllib


@dataclass(frozen=True)
class Experiment:
    start: str
    end: str
    tickers: tuple[str, ...]
    source: str = "flatfiles"
    lookbacks: tuple[int, ...] = (5, 15, 60)
    horizons: tuple[int, ...] = (15, 30, 60)
    skip_minutes: int = 1
    min_cross_section: int = 8
    quantiles: int = 3

    def __post_init__(self):
        for name in ("start", "end"):
            value = getattr(self, name)
            if not isinstance(value, str) or date.fromisoformat(value).isoformat() != value:
                raise ValueError(f"{name} must be a YYYY-MM-DD string")
        if date.fromisoformat(self.start) > date.fromisoformat(self.end):
            raise ValueError("start must be <= end")
        if self.source not in {"flatfiles", "rest_split_adjusted"}:
            raise ValueError("source must be flatfiles or rest_split_adjusted")
        if (not isinstance(self.tickers, (tuple, list)) or not self.tickers
                or any(not isinstance(t, str) or not t or t != t.strip() for t in self.tickers)):
            raise ValueError("Provide a nonempty, explicit ticker list")
        if len(set(self.tickers)) != len(self.tickers):
            raise ValueError("Duplicate tickers in experiment")
        for name in ("lookbacks", "horizons"):
            values = getattr(self, name)
            if (not isinstance(values, (tuple, list)) or not values
                    or any(type(x) is not int or x <= 0 for x in values)
                    or len(values) != len(set(values))):
                raise ValueError(f"{name} must contain distinct positive integers")
        if type(self.skip_minutes) is not int or self.skip_minutes < 1:
            raise ValueError("skip_minutes must be >= 1 after the signal bar closes")
        if type(self.quantiles) is not int or self.quantiles < 2:
            raise ValueError("quantiles must be >= 2")
        if (type(self.min_cross_section) is not int
                or not 2 * self.quantiles <= self.min_cross_section <= len(self.tickers)):
            raise ValueError("Require 2*quantiles <= min_cross_section <= ticker count")

    def to_dict(self):
        return asdict(self)


def load_experiment(path: Path) -> Experiment:
    with path.open("rb") as fh:
        raw = tomllib.load(fh)
    if set(raw) != {"experiment"}:
        raise ValueError("Config must contain only an [experiment] section")
    values = raw["experiment"]
    if not isinstance(values, dict):
        raise ValueError("[experiment] must be a TOML table")
    missing = {"start", "end", "tickers"} - set(values)
    unknown = set(values) - {field.name for field in fields(Experiment)}
    if missing or unknown:
        raise ValueError(f"Invalid experiment fields: missing={sorted(missing)}, unknown={sorted(unknown)}")
    for name in ("tickers", "lookbacks", "horizons"):
        if name in values:
            if not isinstance(values[name], list):
                raise ValueError(f"{name} must be a TOML array")
            values[name] = tuple(values[name])
    return Experiment(**values)
