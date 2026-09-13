"""Semantic tests for calendar boundaries, causality, gaps, and source contracts."""

from dataclasses import replace
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np
import pandas as pd

from us_equity.config import Experiment, load_experiment
from us_equity.data import calendar_for, load_session, minute_grid, normalize_bars
from us_equity.demo import generate_session
from us_equity.evaluate import evaluate_session, summarize
from us_equity.features import compute_features, feature_names
from us_equity.labels import compute_labels
from us_equity.pipeline import build_panel, run


class MinuteResearchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.day = "2025-03-10"
        cls.tickers = tuple(f"SYN{i:02d}" for i in range(12))
        cls.experiment = Experiment(cls.day, cls.day, cls.tickers)
        cls.raw = generate_session(cls.day, cls.tickers)
        cls.grid = minute_grid(calendar_for(cls.day, cls.day), cls.day)

    def linear_bars(self):
        opening = np.arange(len(self.grid), dtype=float) + 100
        return pd.DataFrame({"open": opening, "close": opening + 0.5,
                             "high": opening + 1, "low": opening - 1,
                             "volume": 1000.0}, index=self.grid)

    def test_config_rejects_noncanonical_dates_and_invalid_types(self):
        # Basic ISO dates parse successfully in Python, but cannot be compared
        # lexically with the YYYY-MM-DD input filenames used by discover_files.
        for changes in ({"start": "20250310"}, {"start": 20250310},
                        {"tickers": "ABCDEFGHIJKL"}, {"tickers": (123,)},
                        {"lookbacks": 5}, {"horizons": ([15],)}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                replace(self.experiment, **changes)

    def test_config_loader_rejects_malformed_tables_and_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = Path(tmp) / "experiment.toml"
            for text in ('experiment = "wrong"', '[experiment]\nstart = "2025-03-10"',
                         '[experiment]\nstart="2025-03-10"\nend="2025-03-10"\ntickers="SYN00"',
                         '[experiment]\nstart="2025-03-10"\nend="2025-03-10"\ntickers=[]\ntyop=1'):
                config.write_text(text)
                with self.subTest(text=text), self.assertRaises(ValueError):
                    load_experiment(config)

    def test_dst_half_day_and_holiday(self):
        calendar = calendar_for("2025-03-07", "2025-11-28")
        before = minute_grid(calendar, "2025-03-07")
        after = minute_grid(calendar, "2025-03-10")
        half = minute_grid(calendar, "2025-11-28")
        self.assertEqual((before[0].hour, after[0].hour), (14, 13))
        self.assertEqual((len(before), len(after), len(half)), (390, 390, 210))
        self.assertEqual(half[-1].tz_convert("America/New_York").strftime("%H:%M"), "12:59")
        self.assertFalse(calendar.is_session("2025-07-04"))

    def test_explicit_source_units_and_rest_schemas(self):
        raw = self.raw.iloc[:10].copy()
        expected = normalize_bars(raw, "flatfiles", self.day)
        rest = raw.copy()
        rest["window_start"] //= 1_000_000
        actual = normalize_bars(rest, "rest_split_adjusted", self.day)
        pd.testing.assert_frame_equal(expected, actual)
        short = rest.rename(columns={"window_start": "t", "open": "o", "high": "h",
                                     "low": "l", "close": "c", "volume": "v"})
        pd.testing.assert_frame_equal(expected, normalize_bars(short, "rest_split_adjusted", self.day))
        with self.assertRaisesRegex(ValueError, "Timestamp unit/date mismatch"):
            normalize_bars(rest, "flatfiles", self.day)
        with self.assertRaisesRegex(ValueError, "Timestamp unit/date mismatch"):
            normalize_bars(raw, "rest_split_adjusted", self.day)

    def test_reject_bad_prices_and_wrong_dates(self):
        raw = self.raw.iloc[:5].copy()
        raw.loc[0, "high"] = 0.1
        with self.assertRaisesRegex(ValueError, "OHLC bounds"):
            normalize_bars(raw, "flatfiles", self.day)
        with self.assertRaisesRegex(ValueError, "Timestamp unit/date mismatch"):
            normalize_bars(self.raw, "flatfiles", "2025-03-07")

    def test_signal_close_then_skip_then_entry(self):
        bars = self.linear_bars()
        labels = compute_labels(bars, (15,), 1)
        # Signal at 09:30 closes 09:31, skip [09:31,09:32), entry 09:32, exit 09:47.
        self.assertEqual(labels.entry_proxy_open.iloc[0], 102)
        self.assertAlmostEqual(labels.fwd_15m.iloc[0], 117 / 102 - 1)
        self.assertTrue(labels.fwd_15m.iloc[-17:].isna().all())

    def test_gaps_do_not_compress_time_or_fill_prices(self):
        bars = self.linear_bars()
        bars.iloc[10] = np.nan
        labels = compute_labels(bars, (15,), 1)
        self.assertTrue(pd.isna(labels.fwd_15m.iloc[0]))  # gap inside holding interval
        self.assertTrue(pd.isna(labels.fwd_15m.iloc[10]))  # no signal bar
        self.assertAlmostEqual(labels.fwd_15m.iloc[11], 128 / 113 - 1)
        features = compute_features(bars, (5,))
        self.assertTrue(features.ret_5m.iloc[10:16].isna().all())
        self.assertTrue(pd.notna(features.ret_5m.iloc[16]))

    def test_features_do_not_change_when_future_changes(self):
        bars = self.linear_bars()
        expected = compute_features(bars, (5, 15, 60))
        changed = bars.copy()
        changed.iloc[101:] *= 3
        actual = compute_features(changed, (5, 15, 60))
        pd.testing.assert_frame_equal(expected.iloc[:101], actual.iloc[:101])
        self.assertTrue(expected.ret_60m.iloc[:60].isna().all())

    def test_volume_baseline_excludes_current_minute(self):
        bars = self.linear_bars()
        bars.iloc[30, bars.columns.get_loc("volume")] = 10_000
        self.assertEqual(compute_features(bars, (5,)).volume_ratio_30m.iloc[30], 10)

    def test_missing_ticker_and_no_cross_session_history(self):
        raw = self.raw[self.raw.ticker != self.tickers[-1]]
        bars = normalize_bars(raw, "flatfiles", self.day)
        panel = build_panel(bars, self.grid, self.day, self.experiment)
        absent = panel[panel.ticker == self.tickers[-1]]
        self.assertEqual(len(absent), 390)
        self.assertFalse(absent.observed.any())
        self.assertTrue(absent[feature_names(self.experiment.lookbacks)].isna().all().all())
        first = panel.groupby("ticker").head(1)
        self.assertTrue(first.ret_5m.isna().all())
        self.assertTrue((panel.available_at - panel.ts == pd.Timedelta(minutes=1)).all())

    def test_duplicates_rejected_and_premarket_excluded(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / f"{self.day}.csv"
            duplicate = pd.concat([self.raw, self.raw.iloc[[0]]])
            duplicate.to_csv(path, index=False)
            with self.assertRaisesRegex(ValueError, "Duplicate ticker/minute"):
                load_session([path], self.experiment, self.day, self.grid)
            pre = self.raw.iloc[[0]].copy()
            pre["window_start"] -= 60_000_000_000
            pd.concat([self.raw, pre]).to_csv(path, index=False)
            bars, qa = load_session([path], self.experiment, self.day, self.grid)
            self.assertEqual(len(bars), len(self.raw))
            self.assertEqual(qa["outside_regular_rows"], 1)

    def test_nonoverlap_grid_and_empty_evaluation(self):
        bars = normalize_bars(self.raw, "flatfiles", self.day)
        panel = build_panel(bars, self.grid, self.day, self.experiment)
        buckets = evaluate_session(panel, self.experiment)
        self.assertFalse(buckets.empty)
        for (feature, horizon), group in buckets.groupby(["feature", "horizon"]):
            spacing = group.ts.diff().dropna().dt.total_seconds() / 60
            self.assertTrue((spacing >= horizon).all(), feature)
        buckets = evaluate_session(panel[panel.ticker == self.tickers[0]], self.experiment)
        daily, summary = summarize(buckets)
        self.assertTrue(daily.empty and summary.empty)

    def test_end_to_end_artifacts_and_overwrite_protection(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            raw = root / "raw"
            raw.mkdir()
            self.raw.to_csv(raw / f"{self.day}.csv.gz", index=False)
            report = run(self.experiment, raw, root / "result", synthetic=True)
            manifest = json.loads((report.parent / "manifest.json").read_text())
            self.assertEqual(manifest["status"], "complete")
            self.assertTrue(manifest["synthetic"])
            self.assertEqual(len(manifest["inputs"][0]["sha256"]), 64)
            summary = pd.read_csv(report.parent / "summary.csv")
            self.assertEqual(len(summary), 18)
            self.assertTrue(summary.std_daily_ic.isna().all())
            panel = pd.read_parquet(report.parent / "panels" / f"{self.day}.parquet")
            self.assertEqual(len(panel), 390 * 12)
            self.assertIn("合成数据", report.read_text())
            with self.assertRaises(FileExistsError):
                run(self.experiment, raw, root / "result")
            with self.assertRaisesRegex(FileNotFoundError, "Missing session files"):
                run(replace(self.experiment, end="2025-03-11"), raw, root / "missing")


if __name__ == "__main__":
    unittest.main()
