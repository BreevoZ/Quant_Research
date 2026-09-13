"""Unified Massive/Polygon downloader.

Secrets:
  The script reads REST API keys and Flat Files access keys from KEYS.txt.
  It never prints those keys.

Commands:
  catalog
      Print supported asset classes and data types.

  flatfiles
      Download bulk Flat Files from files.massive.com and extract .csv.gz to CSV.
      This is the right path for full historical/bulk datasets.

  rest
      Download REST data to CSV. REST can run in two modes:

      1) Per-ticker mode, the default:
           One output CSV per ticker.

      2) Combined mode:
           Add --combined-output, usually together with --from-flatfiles.
           The script reads tickers from local Flat Files, downloads REST data
           per ticker into temporary part files, then merges them into one
           date-level CSV, matching the Flat File style.

Common flags:
  --keys KEYS.txt
      Key file path. Default: KEYS.txt.

  --asset stocks,indices
      Asset classes to run. Supports comma-separated values:
      stocks, options, indices, futures, forex, crypto, all.

  --exclude-assets options
      Skip selected asset classes. Useful because options REST full universe is
      enormous. Example: --asset all --exclude-assets options.

  --data day,minute
      Data types to run. For Flat Files this maps to *_aggs_v1/trades_v1/etc.
      For REST this maps to day/minute/trades/quotes.

  --date YYYY-MM-DD
      Single date. Equivalent to --start-date DATE --end-date DATE.

  --start-date YYYY-MM-DD --end-date YYYY-MM-DD
      Historical date range.

  --output-dir downloads/unified
      Output root. Default: downloads/unified.

  --workers 4
      Concurrent request/download workers.

  --request-delay 0.05
      Sleep between task submissions / HTTP attempts. Useful for smoothing API
      rate limits when workers is high.

  --retries 7 --retry-backoff 1.0
      Retry transient HTTP/network failures with exponential backoff.

  --print-every 500
      Progress logging frequency for large ticker universes.

  --overwrite
      Re-download/rebuild existing output files.

  --include-quotes
      Include quotes when --data all is used. By default quotes are excluded
      because stock/options quotes can be huge.

REST-only flags:
  --tickers AAPL,MSFT or C:EURUSD,X:BTCUSD
      Explicit ticker list.

  --all-tickers
      Use reference endpoints to discover tickers. This can be slow.

  --from-flatfiles
      Use local Flat Files as the ticker universe. Run flatfiles first.
      This automatically enables --combined-output in main().

  --combined-output
      Produce one CSV per asset/data/date under downloads/unified/rest_combined.

  --keep-parts
      Keep temporary per-ticker CSV part files under downloads/unified/_parts.

  --max-tickers N
      Limit ticker count for testing.

  --max-pages N
      Limit paginated trades/quotes pages per ticker. Useful for testing; do
      not use it for true full trades/quotes downloads.

  --allow-options-from-flatfiles
      Allow REST combined mode to use every option contract ticker from local
      options Flat Files. This can mean hundreds of thousands of tickers and
      is disabled by default unless --max-tickers or --tickers is used.

Recommended workflows:
  1. Inspect catalog:
       python massive_data_downloader.py catalog

  2. Bulk Flat Files, excluding quotes:
       python massive_data_downloader.py flatfiles --asset all --data all --date 2026-06-18

  3. REST aggregates from Flat File ticker universe, excluding options:
       python massive_data_downloader.py rest --asset all --exclude-assets options --data day,minute --date 2026-06-18 --from-flatfiles --workers 12 --request-delay 0.03

  4. Small REST sample:
       python massive_data_downloader.py rest --asset crypto --data day,minute --tickers X:BTCUSD,X:ETHUSD --date 2026-06-18

Warnings:
  * Full options REST from Flat File ticker universe can mean hundreds of
    thousands of tickers and may run for days. Prefer Flat Files for options,
    or use an underlying/chain snapshot workflow instead.
  * Full REST trades can be dramatically slower than Flat Files. Prefer
    Flat Files for bulk trades.
"""

from __future__ import annotations

import argparse
import csv
import gzip
import json
import os
import pathlib
import re
import shutil
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from datetime import date, datetime, timedelta
from typing import Any, Iterable


API_ROOT = "https://api.massive.com"
FLATFILES_ENDPOINT = "https://files.massive.com"
FLATFILES_BUCKET = "flatfiles"
HTTP_RETRIES = 7
HTTP_RETRY_BACKOFF = 1.0
REQUEST_DELAY = 0.0
PRINT_EVERY = 500


ASSET_ALIASES = {
    "stock": "stocks",
    "stocks": "stocks",
    "equity": "stocks",
    "equities": "stocks",
    "option": "options",
    "options": "options",
    "opra": "options",
    "index": "indices",
    "indices": "indices",
    "future": "futures",
    "futures": "futures",
    "fx": "forex",
    "forex": "forex",
    "currency": "forex",
    "currencies": "forex",
    "crypto": "crypto",
    "cryptos": "crypto",
    "all": "all",
}


DATA_ALIASES = {
    "day": "day_aggs_v1",
    "daily": "day_aggs_v1",
    "day_aggs": "day_aggs_v1",
    "day_aggs_v1": "day_aggs_v1",
    "minute": "minute_aggs_v1",
    "min": "minute_aggs_v1",
    "minute_aggs": "minute_aggs_v1",
    "minute_aggs_v1": "minute_aggs_v1",
    "session": "session_aggs_v1",
    "session_aggs": "session_aggs_v1",
    "session_aggs_v1": "session_aggs_v1",
    "trade": "trades_v1",
    "trades": "trades_v1",
    "trades_v1": "trades_v1",
    "quote": "quotes_v1",
    "quotes": "quotes_v1",
    "quotes_v1": "quotes_v1",
    "all": "all",
}


REST_DATA_ALIASES = {
    "day": "day",
    "daily": "day",
    "minute": "minute",
    "min": "minute",
    "trade": "trades",
    "trades": "trades",
    "quote": "quotes",
    "quotes": "quotes",
    "all": "all",
}


FLATFILE_CATALOG: dict[str, list[dict[str, str | list[str]]]] = {
    "stocks": [
        {
            "prefix": "us_stocks_sip",
            "label": "sip",
            "data_types": ["day_aggs_v1", "minute_aggs_v1", "trades_v1", "quotes_v1"],
        }
    ],
    "options": [
        {
            "prefix": "us_options_opra",
            "label": "opra",
            "data_types": ["day_aggs_v1", "minute_aggs_v1", "trades_v1", "quotes_v1"],
        }
    ],
    "indices": [
        {
            "prefix": "us_indices",
            "label": "us",
            "data_types": ["day_aggs_v1", "minute_aggs_v1"],
        }
    ],
    "futures": [
        {
            "prefix": "us_futures_cme",
            "label": "cme",
            "data_types": ["session_aggs_v1", "minute_aggs_v1", "trades_v1", "quotes_v1"],
        },
        {
            "prefix": "us_futures_cbot",
            "label": "cbot",
            "data_types": ["session_aggs_v1", "minute_aggs_v1", "trades_v1", "quotes_v1"],
        },
        {
            "prefix": "us_futures_comex",
            "label": "comex",
            "data_types": ["session_aggs_v1", "minute_aggs_v1", "trades_v1", "quotes_v1"],
        },
        {
            "prefix": "us_futures_nymex",
            "label": "nymex",
            "data_types": ["session_aggs_v1", "minute_aggs_v1", "trades_v1", "quotes_v1"],
        },
    ],
    "forex": [
        {
            "prefix": "global_forex",
            "label": "global",
            "data_types": ["day_aggs_v1", "minute_aggs_v1", "quotes_v1"],
        }
    ],
    "crypto": [
        {
            "prefix": "global_crypto",
            "label": "global",
            "data_types": ["day_aggs_v1", "minute_aggs_v1", "trades_v1"],
        }
    ],
}


REST_MARKETS = {
    "stocks": "stocks",
    "options": "options",
    "indices": "indices",
    "forex": "fx",
    "crypto": "crypto",
}


REST_SUPPORTED_DATA = {
    "stocks": ["day", "minute", "trades"],
    "options": ["day", "minute", "trades"],
    "indices": ["day", "minute"],
    "futures": ["day", "minute", "trades"],
    "forex": ["day", "minute"],
    "crypto": ["day", "minute", "trades"],
}


DEFAULT_REST_TICKERS = {
    "stocks": ["AAPL", "MSFT", "SPY", "QQQ"],
    "options": [
        "O:A260618C00120000",
        "O:A260618P00125000",
        "O:A260717C00130000",
    ],
    "indices": ["I:SPX", "I:NDX", "I:DJI", "I:RUT"],
    "futures": ["ESM6", "NQM6", "CLN6", "GCQ6"],
    "forex": [
        "C:EURUSD",
        "C:USDJPY",
        "C:GBPUSD",
        "C:AUDUSD",
        "C:USDCAD",
        "C:USDCHF",
        "C:NZDUSD",
        "C:EURJPY",
        "C:USDCNY",
        "C:USDCNH",
    ],
    "crypto": [
        "X:BTCUSD",
        "X:ETHUSD",
        "X:SOLUSD",
        "X:XRPUSD",
        "X:DOGEUSD",
        "X:ADAUSD",
        "X:BNBUSD",
        "X:AVAXUSD",
        "X:LINKUSD",
        "X:LTCUSD",
    ],
}


@dataclass
class KeyStore:
    rest: dict[str, str]
    flatfiles: dict[str, tuple[str, str]]


def parse_date(value: str) -> date:
    return datetime.strptime(value, "%Y-%m-%d").date()


def iter_dates(start: str, end: str | None = None) -> Iterable[date]:
    start_date = parse_date(start)
    end_date = parse_date(end or start)
    if end_date < start_date:
        raise ValueError("--end-date must be >= --start-date")
    current = start_date
    while current <= end_date:
        yield current
        current += timedelta(days=1)


def normalize_asset(asset: str) -> str:
    normalized = ASSET_ALIASES.get(asset.strip().lower())
    if not normalized:
        raise ValueError(f"unknown asset: {asset}")
    return normalized


def split_csv_arg(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def configure_runtime(args: argparse.Namespace) -> None:
    global HTTP_RETRIES, HTTP_RETRY_BACKOFF, REQUEST_DELAY, PRINT_EVERY
    HTTP_RETRIES = max(int(getattr(args, "retries", HTTP_RETRIES)), 1)
    HTTP_RETRY_BACKOFF = max(float(getattr(args, "retry_backoff", HTTP_RETRY_BACKOFF)), 0.0)
    REQUEST_DELAY = max(float(getattr(args, "request_delay", REQUEST_DELAY)), 0.0)
    PRINT_EVERY = max(int(getattr(args, "print_every", PRINT_EVERY)), 1)


def key_group_for_asset(asset: str) -> str:
    return "stocks_options" if asset in {"stocks", "options"} else "other"


def load_keys(path: pathlib.Path) -> KeyStore:
    if not path.exists():
        raise FileNotFoundError(f"key file not found: {path}")

    rest: dict[str, str] = {}
    flatfiles: dict[str, tuple[str, str]] = {}
    section: str | None = None
    group: str | None = None
    pending_flat_access: str | None = None

    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        if not line or set(line) <= {"-"}:
            continue

        upper = line.upper()
        if upper == "REST API":
            section = "rest"
            group = None
            pending_flat_access = None
            continue
        if upper == "FLATFILES":
            section = "flatfiles"
            group = None
            pending_flat_access = None
            continue
        if "OPTION" in upper and "STOCK" in upper:
            group = "stocks_options"
            pending_flat_access = None
            continue
        if "INDEX" in upper or "FUTUR" in upper or "CUR" in upper:
            group = "other"
            pending_flat_access = None
            continue

        if not section or not group:
            continue

        if section == "rest":
            rest[group] = line
        elif section == "flatfiles":
            if pending_flat_access is None:
                pending_flat_access = line
            else:
                flatfiles[group] = (pending_flat_access, line)
                pending_flat_access = None

    return KeyStore(rest=rest, flatfiles=flatfiles)


def get_rest_key(keys: KeyStore, asset: str) -> str:
    group = key_group_for_asset(asset)
    if group not in keys.rest:
        raise RuntimeError(f"REST key for {asset} not found in key file")
    return keys.rest[group]


def safe_ticker(ticker: str) -> str:
    return ticker.replace(":", "_").replace("/", "_").replace("\\", "_")


def normalize_rest_ticker(asset: str, ticker: str) -> str:
    ticker = ticker.strip().upper()
    if asset == "crypto" and ticker and not ticker.startswith("X:"):
        return f"X:{ticker}"
    if asset == "forex" and ticker and not ticker.startswith("C:"):
        return f"C:{ticker}"
    if asset == "indices" and ticker and not ticker.startswith("I:"):
        return f"I:{ticker}"
    return ticker


def flatten_row(row: dict[str, Any]) -> dict[str, Any]:
    flattened: dict[str, Any] = {}
    for key, value in row.items():
        if isinstance(value, (dict, list)):
            flattened[key] = json.dumps(value, ensure_ascii=False, separators=(",", ":"))
        else:
            flattened[key] = value
    return flattened


def write_csv(path: pathlib.Path, rows: list[dict[str, Any]], preferred_columns: list[str] | None = None) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    flattened = [flatten_row(row) for row in rows]

    columns: list[str] = []
    for column in preferred_columns or []:
        if column not in columns:
            columns.append(column)
    for row in flattened:
        for column in row:
            if column not in columns:
                columns.append(column)

    tmp_path = pathlib.Path(str(path) + ".part")
    with tmp_path.open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        for row in flattened:
            writer.writerow(row)
    if path.exists():
        path.unlink()
    tmp_path.replace(path)
    return len(flattened)


def read_csv_header(path: pathlib.Path) -> list[str]:
    with path.open("r", newline="", encoding="utf-8-sig") as handle:
        reader = csv.reader(handle)
        return next(reader, [])


def merge_csv_parts(part_paths: list[pathlib.Path], output_path: pathlib.Path) -> int:
    """Merge ticker-level part CSVs into one date-level CSV without loading all rows."""
    columns: list[str] = []
    existing_parts = [path for path in part_paths if path.exists()]
    for path in existing_parts:
        for column in read_csv_header(path):
            if column and column not in columns:
                columns.append(column)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = pathlib.Path(str(output_path) + ".part")
    row_count = 0
    with tmp_path.open("w", newline="", encoding="utf-8-sig") as target:
        writer = csv.DictWriter(target, fieldnames=columns)
        writer.writeheader()
        for path in existing_parts:
            with path.open("r", newline="", encoding="utf-8-sig") as source:
                reader = csv.DictReader(source)
                for row in reader:
                    writer.writerow(row)
                    row_count += 1
    if output_path.exists():
        output_path.unlink()
    tmp_path.replace(output_path)
    return row_count


def rest_request_json(path: str, params: dict[str, Any], api_key: str) -> dict[str, Any]:
    query = urllib.parse.urlencode({**params, "apiKey": api_key})
    url = f"{API_ROOT}{path}?{query}"
    request = urllib.request.Request(url, headers={"User-Agent": "massive-unified-downloader/1.0"})
    for attempt in range(HTTP_RETRIES):
        try:
            if REQUEST_DELAY > 0:
                time.sleep(REQUEST_DELAY)
            with urllib.request.urlopen(request, timeout=60) as response:
                payload = json.load(response)
            if payload.get("status") not in {"OK", "DELAYED"}:
                raise RuntimeError(f"API status={payload.get('status')}")
            return payload
        except urllib.error.HTTPError as exc:
            if exc.code not in {429, 500, 502, 503, 504} or attempt == HTTP_RETRIES - 1:
                raise
        except (urllib.error.URLError, TimeoutError, ConnectionError):
            if attempt == HTTP_RETRIES - 1:
                raise
        time.sleep(min(HTTP_RETRY_BACKOFF * (2**attempt), 30))
    raise RuntimeError("unreachable")


def rest_pages(path: str, params: dict[str, Any], api_key: str) -> Iterable[list[dict[str, Any]]]:
    url = f"{API_ROOT}{path}?{urllib.parse.urlencode(params)}"
    while url:
        separator = "&" if "?" in url else "?"
        request_url = f"{url}{separator}{urllib.parse.urlencode({'apiKey': api_key})}"
        request = urllib.request.Request(
            request_url, headers={"User-Agent": "massive-unified-downloader/1.0"}
        )
        for attempt in range(HTTP_RETRIES):
            try:
                if REQUEST_DELAY > 0:
                    time.sleep(REQUEST_DELAY)
                with urllib.request.urlopen(request, timeout=60) as response:
                    payload = json.load(response)
                if payload.get("status") not in {"OK", "DELAYED"}:
                    raise RuntimeError(f"API status={payload.get('status')}")
                break
            except urllib.error.HTTPError as exc:
                if exc.code not in {429, 500, 502, 503, 504} or attempt == HTTP_RETRIES - 1:
                    raise
            except (urllib.error.URLError, TimeoutError, ConnectionError):
                if attempt == HTTP_RETRIES - 1:
                    raise
            time.sleep(min(HTTP_RETRY_BACKOFF * (2**attempt), 30))
        yield payload.get("results") or []
        url = payload.get("next_url")


def fetch_reference_tickers(asset: str, run_date: date, api_key: str, limit: int | None = None) -> list[str]:
    if asset == "futures":
        tickers: list[str] = []
        params = {
            "date": run_date.isoformat(),
            "active": "true",
            "limit": 100,
            "sort": "ticker.asc",
        }
        for rows in rest_pages("/futures/v1/contracts", params, api_key):
            tickers.extend(row["ticker"] for row in rows if row.get("ticker"))
            if limit and len(tickers) >= limit:
                return tickers[:limit]
        return tickers

    market = REST_MARKETS.get(asset)
    if not market:
        return []
    tickers = []
    params = {
        "market": market,
        "active": "true",
        "date": run_date.isoformat(),
        "limit": 100,
        "sort": "ticker",
        "order": "asc",
    }
    for rows in rest_pages("/v3/reference/tickers", params, api_key):
        tickers.extend(row["ticker"] for row in rows if row.get("ticker"))
        if limit and len(tickers) >= limit:
            return tickers[:limit]
    return tickers


def expand_assets(asset_arg: str, exclude_arg: str = "") -> list[str]:
    selected: list[str] = []
    for item in split_csv_arg(asset_arg):
        asset = normalize_asset(item)
        if asset == "all":
            selected.extend(["stocks", "options", "indices", "futures", "forex", "crypto"])
        else:
            selected.append(asset)

    if not selected:
        selected = ["stocks", "options", "indices", "futures", "forex", "crypto"]

    excluded = {
        normalize_asset(item)
        for item in split_csv_arg(exclude_arg)
        if normalize_asset(item) != "all"
    }

    result: list[str] = []
    for asset in selected:
        if asset in excluded or asset in result:
            continue
        result.append(asset)
    return result


def expand_flatfile_data_types(asset: str, data_arg: str, include_quotes: bool) -> list[str]:
    requested = split_csv_arg(data_arg)
    if not requested:
        requested = ["all"]
    normalized = [DATA_ALIASES.get(item.lower(), item) for item in requested]
    requested_all = "all" in normalized
    supported = sorted(
        {data_type for entry in FLATFILE_CATALOG[asset] for data_type in entry["data_types"]}  # type: ignore[index]
    )
    if requested_all:
        data_types = supported
    else:
        data_types = [item for item in normalized if item in supported]
        unknown = [item for item in normalized if item not in supported]
        if unknown:
            raise ValueError(f"{asset} does not support flatfile data types: {unknown}")
    if requested_all and not include_quotes:
        data_types = [item for item in data_types if item != "quotes_v1"]
    return data_types


def expand_rest_data_types(asset: str, data_arg: str, include_quotes: bool) -> list[str]:
    requested = split_csv_arg(data_arg)
    if not requested:
        requested = ["all"]
    normalized = [REST_DATA_ALIASES.get(item.lower(), item) for item in requested]
    requested_all = "all" in normalized
    if requested_all:
        data_types = list(REST_SUPPORTED_DATA[asset])
        if include_quotes and asset not in {"futures"}:
            data_types.append("quotes")
    else:
        data_types = normalized
    if requested_all and not include_quotes:
        data_types = [item for item in data_types if item != "quotes"]
    return data_types


def import_boto3():
    try:
        import boto3
        from botocore.config import Config
    except ImportError as exc:
        raise RuntimeError("boto3 is required for flatfiles downloads. Install boto3 first.") from exc
    return boto3, Config


class FlatfileClients:
    def __init__(self, keys: KeyStore):
        self.keys = keys
        self.clients: dict[str, Any] = {}

    def get(self, asset: str):
        group = key_group_for_asset(asset)
        if group not in self.keys.flatfiles:
            raise RuntimeError(f"Flat Files access key for {asset} not found in key file")
        if group not in self.clients:
            boto3, Config = import_boto3()
            access_key, secret_key = self.keys.flatfiles[group]
            self.clients[group] = boto3.client(
                "s3",
                endpoint_url=FLATFILES_ENDPOINT,
                aws_access_key_id=access_key,
                aws_secret_access_key=secret_key,
                config=Config(
                    signature_version="s3v4",
                    retries={"max_attempts": 5, "mode": "adaptive"},
                    max_pool_connections=32,
                    region_name="us-east-2",
                ),
            )
        return self.clients[group]


@dataclass
class FlatfileTask:
    asset: str
    label: str
    prefix: str
    data_type: str
    run_date: date
    output_csv: pathlib.Path
    s3_key: str


def build_flatfile_tasks(args: argparse.Namespace) -> list[FlatfileTask]:
    tasks: list[FlatfileTask] = []
    output_root = pathlib.Path(args.output_dir)
    for asset in expand_assets(args.asset, args.exclude_assets):
        data_types = expand_flatfile_data_types(asset, args.data, args.include_quotes)
        for run_date in iter_dates(args.start_date, args.end_date):
            yyyy = f"{run_date.year:04d}"
            mm = f"{run_date.month:02d}"
            for entry in FLATFILE_CATALOG[asset]:
                prefix = str(entry["prefix"])
                label = str(entry["label"])
                supported = set(entry["data_types"])  # type: ignore[arg-type]
                for data_type in data_types:
                    if data_type not in supported:
                        continue
                    s3_key = f"{prefix}/{data_type}/{yyyy}/{mm}/{run_date.isoformat()}.csv.gz"
                    parts = [output_root, pathlib.Path("flatfiles"), pathlib.Path(asset)]
                    if asset == "futures":
                        parts.append(pathlib.Path(label))
                    parts.extend([pathlib.Path(data_type), pathlib.Path(yyyy), pathlib.Path(mm)])
                    output_csv = pathlib.Path(*parts) / f"{run_date.isoformat()}.csv"
                    tasks.append(
                        FlatfileTask(
                            asset=asset,
                            label=label,
                            prefix=prefix,
                            data_type=data_type,
                            run_date=run_date,
                            output_csv=output_csv,
                            s3_key=s3_key,
                        )
                    )
    return tasks


def download_flatfile_task(
    clients: FlatfileClients,
    task: FlatfileTask,
    overwrite: bool,
    keep_gz: bool,
) -> tuple[FlatfileTask, str, str]:
    if task.output_csv.exists() and not overwrite:
        return task, "skipped", "csv exists"

    task.output_csv.parent.mkdir(parents=True, exist_ok=True)
    gz_part = pathlib.Path(str(task.output_csv) + ".gz.part")
    csv_part = pathlib.Path(str(task.output_csv) + ".part")
    final_gz = pathlib.Path(str(task.output_csv) + ".gz")

    for path in (gz_part, csv_part):
        if path.exists():
            path.unlink()

    client = clients.get(task.asset)
    try:
        response = client.get_object(Bucket=FLATFILES_BUCKET, Key=task.s3_key)
        with gz_part.open("wb") as handle:
            shutil.copyfileobj(response["Body"], handle)
    except Exception as exc:
        if gz_part.exists():
            gz_part.unlink()
        error_text = f"{type(exc).__name__}: {exc}"
        if any(code in error_text for code in ("NoSuchKey", "404", "Not Found")):
            return task, "skipped", "missing flatfile"
        return task, "failed", f"{type(exc).__name__}: {exc}"

    try:
        with gzip.open(gz_part, "rb") as source, csv_part.open("wb") as target:
            shutil.copyfileobj(source, target)
        if task.output_csv.exists() and overwrite:
            task.output_csv.unlink()
        csv_part.replace(task.output_csv)
        if keep_gz:
            if final_gz.exists() and overwrite:
                final_gz.unlink()
            gz_part.replace(final_gz)
        else:
            gz_part.unlink()
        return task, "ok", ""
    except Exception as exc:
        for path in (gz_part, csv_part):
            if path.exists():
                path.unlink()
        return task, "failed", f"extract {type(exc).__name__}: {exc}"


def run_flatfiles(args: argparse.Namespace) -> int:
    configure_runtime(args)
    keys = load_keys(pathlib.Path(args.keys))
    tasks = build_flatfile_tasks(args)
    if not tasks:
        print("No flatfile tasks generated.")
        return 0

    print(f"Flat Files tasks={len(tasks):,} output={args.output_dir}")
    if not args.include_quotes:
        print("quotes_v1 is excluded from data=all; pass --include-quotes or --data quotes to include it.")

    clients = FlatfileClients(keys)
    success = skipped = failed = 0
    failures: list[dict[str, str]] = []
    with ThreadPoolExecutor(max_workers=args.workers) as executor:
        futures = {}
        for number, task in enumerate(tasks, 1):
            future = executor.submit(download_flatfile_task, clients, task, args.overwrite, args.keep_gz)
            futures[future] = task
            if REQUEST_DELAY > 0 and number < len(tasks):
                time.sleep(REQUEST_DELAY)
        for number, future in enumerate(as_completed(futures), 1):
            task, status, message = future.result()
            if status == "ok":
                success += 1
            elif status == "skipped":
                skipped += 1
            else:
                failed += 1
                failures.append(
                    {
                        "asset": task.asset,
                        "label": task.label,
                        "data_type": task.data_type,
                        "date": task.run_date.isoformat(),
                        "s3_key": task.s3_key,
                        "error": message,
                    }
                )
            if status == "failed" or number <= 5 or number == len(tasks) or number % PRINT_EVERY == 0:
                print(
                    f"[{number:,}/{len(tasks):,}] {status.upper()} "
                    f"{task.asset}/{task.label}/{task.data_type}/{task.run_date} {message}",
                    flush=True,
                )

    print(f"Flat Files complete: ok={success:,} skipped={skipped:,} failed={failed:,}")
    if failures:
        failure_path = pathlib.Path(args.output_dir) / "flatfiles_failures.json"
        failure_path.parent.mkdir(parents=True, exist_ok=True)
        failure_path.write_text(json.dumps(failures, indent=2, ensure_ascii=False), encoding="utf-8")
        print(f"Failures written to {failure_path}")
        return 1
    return 0


def rest_output_path(
    output_root: pathlib.Path,
    asset: str,
    data_type: str,
    run_date: date,
    ticker: str | None = None,
) -> pathlib.Path:
    yyyy = f"{run_date.year:04d}"
    mm = f"{run_date.month:02d}"
    directory = output_root / "rest" / asset / data_type / yyyy / mm
    if ticker:
        return directory / f"{run_date.isoformat()}_{safe_ticker(ticker)}.csv"
    return directory / f"{run_date.isoformat()}.csv"


def rest_data_label(asset: str, data_type: str) -> str:
    if asset == "futures" and data_type == "day":
        return "session_aggs"
    if data_type == "day":
        return "day_aggs"
    if data_type == "minute":
        return "minute_aggs"
    return data_type


def rest_combined_output_path(
    output_root: pathlib.Path, asset: str, data_type: str, run_date: date
) -> pathlib.Path:
    yyyy = f"{run_date.year:04d}"
    mm = f"{run_date.month:02d}"
    return (
        output_root
        / "rest_combined"
        / asset
        / rest_data_label(asset, data_type)
        / yyyy
        / mm
        / f"{run_date.isoformat()}.csv"
    )


def rest_combined_part_path(
    output_root: pathlib.Path, asset: str, data_type: str, run_date: date, ticker: str
) -> pathlib.Path:
    yyyy = f"{run_date.year:04d}"
    mm = f"{run_date.month:02d}"
    return (
        output_root
        / "_parts"
        / "rest_combined"
        / asset
        / rest_data_label(asset, data_type)
        / yyyy
        / mm
        / run_date.isoformat()
        / f"{safe_ticker(ticker)}.csv"
    )


def flatfile_ticker_source_paths(
    output_root: pathlib.Path, asset: str, data_type: str, run_date: date
) -> list[pathlib.Path]:
    """Return local Flat File CSVs used to define REST ticker universe."""
    yyyy = f"{run_date.year:04d}"
    mm = f"{run_date.month:02d}"
    base = output_root / "flatfiles"

    # Prefer small daily/session files for ticker discovery. They are much
    # cheaper to scan than trades files and reflect the active symbols that
    # actually have data on the date.
    if asset == "futures":
        paths = [
            base / "futures" / venue / "session_aggs_v1" / yyyy / mm / f"{run_date.isoformat()}.csv"
            for venue in ("cme", "cbot", "comex", "nymex")
        ]
        existing = [path for path in paths if path.exists()]
        if existing:
            return existing
        return [
            base / "futures" / venue / "minute_aggs_v1" / yyyy / mm / f"{run_date.isoformat()}.csv"
            for venue in ("cme", "cbot", "comex", "nymex")
        ]

    preferred = base / asset / "day_aggs_v1" / yyyy / mm / f"{run_date.isoformat()}.csv"
    if preferred.exists():
        return [preferred]

    fallback_type = {
        "day": "day_aggs_v1",
        "minute": "minute_aggs_v1",
        "trades": "trades_v1",
    }.get(data_type, "day_aggs_v1")
    return [base / asset / fallback_type / yyyy / mm / f"{run_date.isoformat()}.csv"]


def read_tickers_from_flatfiles(
    output_root: pathlib.Path,
    asset: str,
    data_type: str,
    run_date: date,
    max_tickers: int | None = None,
) -> list[str]:
    paths = flatfile_ticker_source_paths(output_root, asset, data_type, run_date)
    tickers: list[str] = []
    seen: set[str] = set()
    missing = [path for path in paths if not path.exists()]
    if len(missing) == len(paths):
        raise FileNotFoundError(
            "No local flatfile ticker source found. Run flatfiles first or pass --tickers. "
            f"Tried: {', '.join(str(path) for path in paths)}"
        )

    for path in paths:
        if not path.exists():
            continue
        with path.open("r", newline="", encoding="utf-8-sig") as handle:
            reader = csv.DictReader(handle)
            for row in reader:
                ticker = row.get("ticker") or row.get("T")
                if not ticker or ticker in seen:
                    continue
                seen.add(ticker)
                tickers.append(normalize_rest_ticker(asset, ticker))
                if max_tickers and len(tickers) >= max_tickers:
                    return tickers
    return tickers


def normalize_aggregate_row(ticker: str, row: dict[str, Any]) -> dict[str, Any]:
    return {
        "ticker": ticker,
        "volume": row.get("v"),
        "open": row.get("o"),
        "close": row.get("c"),
        "high": row.get("h"),
        "low": row.get("l"),
        "window_start": row.get("t"),
        "transactions": row.get("n"),
        "volume_weighted_average_price": row.get("vw"),
    }


def fetch_rest_aggregate_rows(
    ticker: str, timespan: str, run_date: date, api_key: str
) -> list[dict[str, Any]]:
    encoded_ticker = urllib.parse.quote(ticker, safe="")
    payload = rest_request_json(
        f"/v2/aggs/ticker/{encoded_ticker}/range/1/{timespan}/{run_date.isoformat()}/{run_date.isoformat()}",
        {"adjusted": "true", "sort": "asc", "limit": 50000},
        api_key,
    )
    return [normalize_aggregate_row(ticker, row) for row in payload.get("results") or []]


def fetch_rest_stock_grouped_day_rows(run_date: date, api_key: str) -> list[dict[str, Any]]:
    payload = rest_request_json(
        f"/v2/aggs/grouped/locale/us/market/stocks/{run_date.isoformat()}",
        {"adjusted": "true"},
        api_key,
    )
    return [
        normalize_aggregate_row(row.get("T", ""), row)
        for row in payload.get("results") or []
    ]


def fetch_rest_event_rows(
    ticker: str,
    event_type: str,
    run_date: date,
    api_key: str,
    max_pages: int | None,
) -> list[dict[str, Any]]:
    endpoint = "trades" if event_type == "trades" else "quotes"
    encoded_ticker = urllib.parse.quote(ticker, safe="")
    params: dict[str, Any] = {
        "timestamp": run_date.isoformat(),
        "sort": "timestamp",
        "order": "asc",
        "limit": 50000,
    }
    rows: list[dict[str, Any]] = []
    for page_number, page in enumerate(rest_pages(f"/v3/{endpoint}/{encoded_ticker}", params, api_key), 1):
        for row in page:
            row["ticker"] = ticker
        rows.extend(page)
        if max_pages and page_number >= max_pages:
            break
    return rows


def fetch_rest_futures_aggregate_rows(
    ticker: str, timespan: str, run_date: date, api_key: str
) -> list[dict[str, Any]]:
    resolution = "1session" if timespan == "day" else "1min"
    encoded_ticker = urllib.parse.quote(ticker, safe="")
    params = {
        "resolution": resolution,
        "window_start.gte": run_date.isoformat(),
        "window_start.lt": (run_date + timedelta(days=1)).isoformat(),
        "sort": "window_start.asc",
        "limit": 50000,
    }
    rows: list[dict[str, Any]] = []
    for page in rest_pages(f"/futures/v1/aggs/{encoded_ticker}", params, api_key):
        for row in page:
            row["ticker"] = ticker
            row["resolution"] = resolution
        rows.extend(page)
    return rows


def fetch_rest_futures_trade_rows(
    ticker: str, run_date: date, api_key: str, max_pages: int | None
) -> list[dict[str, Any]]:
    encoded_ticker = urllib.parse.quote(ticker, safe="")
    params = {"session_end_date": run_date.isoformat(), "limit": 50000}
    rows: list[dict[str, Any]] = []
    for page_number, page in enumerate(rest_pages(f"/futures/v1/trades/{encoded_ticker}", params, api_key), 1):
        for row in page:
            row["ticker"] = ticker
        rows.extend(page)
        if max_pages and page_number >= max_pages:
            break
    return rows


def download_rest_stock_grouped_day(
    asset: str, run_date: date, api_key: str, output_root: pathlib.Path, overwrite: bool
) -> tuple[str, str]:
    path = rest_output_path(output_root, asset, "grouped_day", run_date)
    if path.exists() and not overwrite:
        return "skipped", str(path)
    payload = rest_request_json(
        f"/v2/aggs/grouped/locale/us/market/stocks/{run_date.isoformat()}",
        {"adjusted": "true"},
        api_key,
    )
    rows = payload.get("results") or []
    write_csv(
        path,
        rows,
        ["T", "o", "h", "l", "c", "v", "vw", "t", "n"],
    )
    return "ok", f"{path} rows={len(rows):,}"


def download_rest_aggregates(
    asset: str,
    ticker: str,
    timespan: str,
    run_date: date,
    api_key: str,
    output_root: pathlib.Path,
    overwrite: bool,
) -> tuple[str, str]:
    path = rest_output_path(output_root, asset, f"{timespan}_aggs", run_date, ticker)
    if path.exists() and not overwrite:
        return "skipped", str(path)

    encoded_ticker = urllib.parse.quote(ticker, safe="")
    payload = rest_request_json(
        f"/v2/aggs/ticker/{encoded_ticker}/range/1/{timespan}/{run_date.isoformat()}/{run_date.isoformat()}",
        {"adjusted": "true", "sort": "asc", "limit": 50000},
        api_key,
    )
    rows = payload.get("results") or []
    for row in rows:
        row["ticker"] = ticker
    write_csv(
        path,
        rows,
        ["ticker", "t", "o", "h", "l", "c", "v", "vw", "n"],
    )
    return "ok", f"{path} rows={len(rows):,}"


def download_rest_events(
    asset: str,
    ticker: str,
    event_type: str,
    run_date: date,
    api_key: str,
    output_root: pathlib.Path,
    overwrite: bool,
    max_pages: int | None,
) -> tuple[str, str]:
    path = rest_output_path(output_root, asset, event_type, run_date, ticker)
    if path.exists() and not overwrite:
        return "skipped", str(path)

    endpoint = "trades" if event_type == "trades" else "quotes"
    encoded_ticker = urllib.parse.quote(ticker, safe="")
    params: dict[str, Any] = {
        "timestamp": run_date.isoformat(),
        "sort": "timestamp",
        "order": "asc",
        "limit": 50000,
    }
    rows: list[dict[str, Any]] = []
    for page_number, page in enumerate(rest_pages(f"/v3/{endpoint}/{encoded_ticker}", params, api_key), 1):
        for row in page:
            row["ticker"] = ticker
        rows.extend(page)
        if max_pages and page_number >= max_pages:
            break
    write_csv(path, rows, ["ticker"])
    return "ok", f"{path} rows={len(rows):,}"


def download_rest_futures_aggregates(
    ticker: str,
    timespan: str,
    run_date: date,
    api_key: str,
    output_root: pathlib.Path,
    overwrite: bool,
) -> tuple[str, str]:
    resolution = "1session" if timespan == "day" else "1min"
    data_label = "session_aggs" if timespan == "day" else "minute_aggs"
    path = rest_output_path(output_root, "futures", data_label, run_date, ticker)
    if path.exists() and not overwrite:
        return "skipped", str(path)

    encoded_ticker = urllib.parse.quote(ticker, safe="")
    params = {
        "resolution": resolution,
        "window_start.gte": run_date.isoformat(),
        "window_start.lt": (run_date + timedelta(days=1)).isoformat(),
        "sort": "window_start.asc",
        "limit": 50000,
    }
    rows: list[dict[str, Any]] = []
    for page in rest_pages(f"/futures/v1/aggs/{encoded_ticker}", params, api_key):
        for row in page:
            row["ticker"] = ticker
            row["resolution"] = resolution
        rows.extend(page)
    write_csv(path, rows, ["ticker", "resolution"])
    return "ok", f"{path} rows={len(rows):,}"


def download_rest_futures_trades(
    ticker: str,
    run_date: date,
    api_key: str,
    output_root: pathlib.Path,
    overwrite: bool,
    max_pages: int | None,
) -> tuple[str, str]:
    path = rest_output_path(output_root, "futures", "trades", run_date, ticker)
    if path.exists() and not overwrite:
        return "skipped", str(path)

    encoded_ticker = urllib.parse.quote(ticker, safe="")
    params = {"session_end_date": run_date.isoformat(), "limit": 50000}
    rows: list[dict[str, Any]] = []
    for page_number, page in enumerate(rest_pages(f"/futures/v1/trades/{encoded_ticker}", params, api_key), 1):
        for row in page:
            row["ticker"] = ticker
        rows.extend(page)
        if max_pages and page_number >= max_pages:
            break
    write_csv(path, rows, ["ticker"])
    return "ok", f"{path} rows={len(rows):,}"


@dataclass
class RestTask:
    asset: str
    ticker: str | None
    data_type: str
    run_date: date


@dataclass
class RestCombinedDataset:
    asset: str
    data_type: str
    run_date: date
    tickers: list[str]


def build_rest_tasks(args: argparse.Namespace, keys: KeyStore) -> list[RestTask]:
    assets = expand_assets(args.asset, args.exclude_assets)
    output_tasks: list[RestTask] = []
    for asset in assets:
        data_types = expand_rest_data_types(asset, args.data, args.include_quotes)
        for run_date in iter_dates(args.start_date, args.end_date):
            if asset == "stocks" and "day" in data_types and not args.tickers and not args.all_tickers:
                output_tasks.append(RestTask(asset=asset, ticker=None, data_type="grouped_day", run_date=run_date))
                data_types = [data_type for data_type in data_types if data_type != "day"]

            if args.tickers:
                tickers = [
                    normalize_rest_ticker(asset, ticker)
                    for ticker in split_csv_arg(args.tickers)
                ]
            elif args.all_tickers:
                api_key = get_rest_key(keys, asset)
                tickers = fetch_reference_tickers(asset, run_date, api_key, args.max_tickers)
            else:
                tickers = DEFAULT_REST_TICKERS.get(asset, [])

            if not tickers and data_types:
                print(f"REST {asset}: no tickers selected; use --tickers or --all-tickers")
                continue

            for ticker in tickers:
                for data_type in data_types:
                    output_tasks.append(RestTask(asset=asset, ticker=ticker, data_type=data_type, run_date=run_date))
    return output_tasks


def build_rest_combined_datasets(args: argparse.Namespace, keys: KeyStore) -> list[RestCombinedDataset]:
    output_root = pathlib.Path(args.output_dir)
    datasets: list[RestCombinedDataset] = []
    for asset in expand_assets(args.asset, args.exclude_assets):
        data_types = expand_rest_data_types(asset, args.data, args.include_quotes)
        for run_date in iter_dates(args.start_date, args.end_date):
            for data_type in data_types:
                if (
                    asset == "options"
                    and args.from_flatfiles
                    and not args.tickers
                    and not args.max_tickers
                    and not args.allow_options_from_flatfiles
                ):
                    print(
                        "REST combined options skipped: local option Flat Files contain "
                        "contract-level tickers and can produce hundreds of thousands of "
                        "REST requests. Use --max-tickers for a sample or "
                        "--allow-options-from-flatfiles to force it."
                    )
                    continue

                if asset == "stocks" and data_type == "day" and not args.tickers:
                    datasets.append(
                        RestCombinedDataset(asset=asset, data_type=data_type, run_date=run_date, tickers=[])
                    )
                    continue

                if args.tickers:
                    tickers = [
                        normalize_rest_ticker(asset, ticker)
                        for ticker in split_csv_arg(args.tickers)
                    ]
                elif args.from_flatfiles:
                    tickers = read_tickers_from_flatfiles(
                        output_root, asset, data_type, run_date, args.max_tickers
                    )
                elif args.all_tickers:
                    api_key = get_rest_key(keys, asset)
                    tickers = fetch_reference_tickers(asset, run_date, api_key, args.max_tickers)
                else:
                    tickers = DEFAULT_REST_TICKERS.get(asset, [])
                    if args.max_tickers:
                        tickers = tickers[: args.max_tickers]

                if not tickers:
                    print(f"REST combined {asset}/{data_type}/{run_date}: no tickers selected")
                    continue
                datasets.append(
                    RestCombinedDataset(asset=asset, data_type=data_type, run_date=run_date, tickers=tickers)
                )
    return datasets


def fetch_rest_combined_part(
    dataset: RestCombinedDataset,
    ticker: str,
    api_key: str,
    output_root: pathlib.Path,
    overwrite: bool,
    max_pages: int | None,
) -> tuple[str, str, str]:
    part_path = rest_combined_part_path(
        output_root, dataset.asset, dataset.data_type, dataset.run_date, ticker
    )
    if part_path.exists() and not overwrite:
        return ticker, "skipped", str(part_path)

    if dataset.asset == "futures":
        if dataset.data_type in {"day", "minute"}:
            rows = fetch_rest_futures_aggregate_rows(
                ticker, dataset.data_type, dataset.run_date, api_key
            )
            preferred = ["ticker", "resolution"]
        elif dataset.data_type == "trades":
            rows = fetch_rest_futures_trade_rows(ticker, dataset.run_date, api_key, max_pages)
            preferred = ["ticker"]
        else:
            raise RuntimeError("futures REST quotes are not implemented")
    elif dataset.data_type in {"day", "minute"}:
        rows = fetch_rest_aggregate_rows(ticker, dataset.data_type, dataset.run_date, api_key)
        preferred = [
            "ticker",
            "volume",
            "open",
            "close",
            "high",
            "low",
            "window_start",
            "transactions",
            "volume_weighted_average_price",
        ]
    elif dataset.data_type in {"trades", "quotes"}:
        rows = fetch_rest_event_rows(
            ticker, dataset.data_type, dataset.run_date, api_key, max_pages
        )
        preferred = ["ticker"]
    else:
        raise RuntimeError(f"unknown REST data type: {dataset.data_type}")

    write_csv(part_path, rows, preferred)
    return ticker, "ok", f"{part_path} rows={len(rows):,}"


def run_rest_combined_dataset(
    dataset: RestCombinedDataset,
    keys: KeyStore,
    output_root: pathlib.Path,
    workers: int,
    overwrite: bool,
    max_pages: int | None,
    keep_parts: bool,
) -> tuple[RestCombinedDataset, str, str]:
    output_path = rest_combined_output_path(output_root, dataset.asset, dataset.data_type, dataset.run_date)
    if output_path.exists() and not overwrite:
        return dataset, "skipped", str(output_path)

    api_key = get_rest_key(keys, dataset.asset)

    if dataset.asset == "stocks" and dataset.data_type == "day" and not dataset.tickers:
        rows = fetch_rest_stock_grouped_day_rows(dataset.run_date, api_key)
        write_csv(
            output_path,
            rows,
            [
                "ticker",
                "volume",
                "open",
                "close",
                "high",
                "low",
                "window_start",
                "transactions",
                "volume_weighted_average_price",
            ],
        )
        return dataset, "ok", f"{output_path} rows={len(rows):,}"

    part_paths = [
        rest_combined_part_path(output_root, dataset.asset, dataset.data_type, dataset.run_date, ticker)
        for ticker in dataset.tickers
    ]
    failures: list[dict[str, str]] = []
    with ThreadPoolExecutor(max_workers=workers) as executor:
        futures = {}
        for number, ticker in enumerate(dataset.tickers, 1):
            future = executor.submit(
                fetch_rest_combined_part,
                dataset,
                ticker,
                api_key,
                output_root,
                overwrite,
                max_pages,
            )
            futures[future] = ticker
            if REQUEST_DELAY > 0 and number < len(dataset.tickers):
                time.sleep(REQUEST_DELAY)

        for number, future in enumerate(as_completed(futures), 1):
            ticker = futures[future]
            try:
                _, status, message = future.result()
            except Exception as exc:
                status = "failed"
                message = f"{type(exc).__name__}: {exc}"
                failures.append({"ticker": ticker, "error": message})
            should_print = (
                status == "failed"
                or number <= 5
                or number == len(dataset.tickers)
                or number % PRINT_EVERY == 0
            )
            if should_print:
                print(
                    f"    [{number:,}/{len(dataset.tickers):,}] {status.upper()} {ticker} {message}",
                    flush=True,
                )

    if failures:
        failure_path = output_path.parent / f"{output_path.stem}.failures.json"
        failure_path.write_text(json.dumps(failures, indent=2, ensure_ascii=False), encoding="utf-8")
        return dataset, "failed", f"{len(failures):,} ticker failures; see {failure_path}"

    rows = merge_csv_parts(part_paths, output_path)
    if not keep_parts:
        part_dir = rest_combined_part_path(
            output_root,
            dataset.asset,
            dataset.data_type,
            dataset.run_date,
            dataset.tickers[0],
        ).parent
        shutil.rmtree(part_dir, ignore_errors=True)
    return dataset, "ok", f"{output_path} rows={rows:,} tickers={len(dataset.tickers):,}"


def run_rest_combined(args: argparse.Namespace) -> int:
    configure_runtime(args)
    keys = load_keys(pathlib.Path(args.keys))
    datasets = build_rest_combined_datasets(args, keys)
    if not datasets:
        print("No REST combined datasets generated.")
        return 0

    output_root = pathlib.Path(args.output_dir)
    print(f"REST combined datasets={len(datasets):,} output={output_root / 'rest_combined'}")
    if args.from_flatfiles:
        print("Ticker universe: local flatfiles")
    if not args.include_quotes:
        print("quotes are excluded from data=all; pass --include-quotes or --data quotes to include them.")

    success = skipped = failed = 0
    failures: list[dict[str, str]] = []
    for number, dataset in enumerate(datasets, 1):
        ticker_info = "grouped" if not dataset.tickers else f"tickers={len(dataset.tickers):,}"
        print(
            f"[{number:,}/{len(datasets):,}] {dataset.asset}/{dataset.data_type}/{dataset.run_date} {ticker_info}",
            flush=True,
        )
        try:
            _, status, message = run_rest_combined_dataset(
                dataset,
                keys,
                output_root,
                args.workers,
                args.overwrite,
                args.max_pages,
                args.keep_parts,
            )
        except Exception as exc:
            status = "failed"
            message = f"{type(exc).__name__}: {exc}"

        if status == "ok":
            success += 1
        elif status == "skipped":
            skipped += 1
        else:
            failed += 1
            failures.append(
                {
                    "asset": dataset.asset,
                    "data_type": dataset.data_type,
                    "date": dataset.run_date.isoformat(),
                    "error": message,
                }
            )
        print(f"  => {status.upper()} {message}", flush=True)

    print(f"REST combined complete: ok={success:,} skipped={skipped:,} failed={failed:,}")
    failure_path = output_root / "rest_combined_failures.json"
    if failures:
        failure_path.write_text(json.dumps(failures, indent=2, ensure_ascii=False), encoding="utf-8")
        print(f"Failures written to {failure_path}")
        return 1
    if failure_path.exists():
        failure_path.unlink()
    return 0


def run_rest_task(
    task: RestTask,
    keys: KeyStore,
    output_root: pathlib.Path,
    overwrite: bool,
    max_pages: int | None,
) -> tuple[RestTask, str, str]:
    api_key = get_rest_key(keys, task.asset)
    try:
        if task.asset == "stocks" and task.data_type == "grouped_day":
            status, message = download_rest_stock_grouped_day(
                task.asset, task.run_date, api_key, output_root, overwrite
            )
        elif task.asset == "futures":
            if task.ticker is None:
                raise RuntimeError("futures REST task missing ticker")
            if task.data_type in {"day", "minute"}:
                status, message = download_rest_futures_aggregates(
                    task.ticker, task.data_type, task.run_date, api_key, output_root, overwrite
                )
            elif task.data_type == "trades":
                status, message = download_rest_futures_trades(
                    task.ticker, task.run_date, api_key, output_root, overwrite, max_pages
                )
            else:
                raise RuntimeError("futures REST quotes are not implemented; use flatfiles if entitled")
        else:
            if task.ticker is None:
                raise RuntimeError("REST task missing ticker")
            if task.data_type in {"day", "minute"}:
                status, message = download_rest_aggregates(
                    task.asset,
                    task.ticker,
                    task.data_type,
                    task.run_date,
                    api_key,
                    output_root,
                    overwrite,
                )
            elif task.data_type in {"trades", "quotes"}:
                status, message = download_rest_events(
                    task.asset,
                    task.ticker,
                    task.data_type,
                    task.run_date,
                    api_key,
                    output_root,
                    overwrite,
                    max_pages,
                )
            else:
                raise RuntimeError(f"unknown REST data type: {task.data_type}")
        return task, status, message
    except Exception as exc:
        return task, "failed", f"{type(exc).__name__}: {exc}"


def run_rest(args: argparse.Namespace) -> int:
    configure_runtime(args)
    keys = load_keys(pathlib.Path(args.keys))
    tasks = build_rest_tasks(args, keys)
    if not tasks:
        print("No REST tasks generated.")
        return 0

    output_root = pathlib.Path(args.output_dir)
    print(f"REST tasks={len(tasks):,} output={output_root}")
    if not args.include_quotes:
        print("quotes are excluded from data=all; pass --include-quotes or --data quotes to include them.")

    success = skipped = failed = 0
    failures: list[dict[str, str]] = []
    with ThreadPoolExecutor(max_workers=args.workers) as executor:
        futures = {}
        for number, task in enumerate(tasks, 1):
            future = executor.submit(run_rest_task, task, keys, output_root, args.overwrite, args.max_pages)
            futures[future] = task
            if REQUEST_DELAY > 0 and number < len(tasks):
                time.sleep(REQUEST_DELAY)
        for number, future in enumerate(as_completed(futures), 1):
            task, status, message = future.result()
            if status == "ok":
                success += 1
            elif status == "skipped":
                skipped += 1
            else:
                failed += 1
                failures.append(
                    {
                        "asset": task.asset,
                        "ticker": task.ticker or "",
                        "data_type": task.data_type,
                        "date": task.run_date.isoformat(),
                        "error": message,
                    }
                )
            ticker = task.ticker or "ALL"
            if status == "failed" or number <= 5 or number == len(tasks) or number % PRINT_EVERY == 0:
                print(
                    f"[{number:,}/{len(tasks):,}] {status.upper()} "
                    f"{task.asset}/{ticker}/{task.data_type}/{task.run_date} {message}",
                    flush=True,
                )

    print(f"REST complete: ok={success:,} skipped={skipped:,} failed={failed:,}")
    if failures:
        failure_path = output_root / "rest_failures.json"
        failure_path.parent.mkdir(parents=True, exist_ok=True)
        failure_path.write_text(json.dumps(failures, indent=2, ensure_ascii=False), encoding="utf-8")
        print(f"Failures written to {failure_path}")
        return 1
    return 0


def print_catalog() -> int:
    print("Flat Files catalog")
    for asset, entries in FLATFILE_CATALOG.items():
        print(f"  {asset}")
        for entry in entries:
            data_types = ", ".join(entry["data_types"])  # type: ignore[arg-type]
            print(f"    {entry['label']}: {entry['prefix']} -> {data_types}")

    print("\nREST catalog")
    print("  stocks: grouped day without --tickers; day/minute/trades with --tickers or --all-tickers")
    print("  options: day/minute/trades with --tickers")
    print("  indices: day/minute with --tickers or --all-tickers")
    print("  futures: day=session aggregates, minute aggregates, trades with --tickers or --all-tickers")
    print("  forex: day/minute with --tickers; default sample includes CNY/CNH")
    print("  crypto: day/minute/trades with --tickers; default sample includes major USD pairs")
    print("  quotes: excluded from --data all by default; pass --data quotes or --include-quotes when needed")
    print("  combined mode: pass --from-flatfiles --combined-output to read tickers from local Flat Files")
    return 0


def add_common_download_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--keys", default="KEYS.txt", help="Key file path. Default: KEYS.txt")
    parser.add_argument(
        "--asset",
        default="all",
        help="Asset classes: stocks,options,indices,futures,forex,crypto,all. Comma-separated.",
    )
    parser.add_argument(
        "--exclude-assets",
        default="",
        help="Comma-separated assets to skip, e.g. --exclude-assets options",
    )
    parser.add_argument(
        "--data",
        default="all",
        help=(
            "Data types. Flat Files: day,minute,session,trades,quotes,all. "
            "REST: day,minute,trades,quotes,all."
        ),
    )
    parser.add_argument("--date", help="Single date YYYY-MM-DD")
    parser.add_argument("--start-date", help="Range start date YYYY-MM-DD")
    parser.add_argument("--end-date", help="Range end date YYYY-MM-DD")
    parser.add_argument("--output-dir", default="downloads/unified", help="Output root directory")
    parser.add_argument("--workers", type=int, default=4, help="Concurrent workers. Default: 4")
    parser.add_argument(
        "--request-delay",
        type=float,
        default=0.0,
        help="Seconds to sleep between task submissions / HTTP attempts. Default: 0",
    )
    parser.add_argument(
        "--retries",
        type=int,
        default=7,
        help="Retry count for transient HTTP/network errors. Default: 7",
    )
    parser.add_argument(
        "--retry-backoff",
        type=float,
        default=1.0,
        help="Exponential retry backoff base seconds. Default: 1.0",
    )
    parser.add_argument(
        "--print-every",
        type=int,
        default=500,
        help="Progress print frequency for large tasks. Default: 500",
    )
    parser.add_argument("--overwrite", action="store_true", help="Overwrite existing outputs")
    parser.add_argument(
        "--include-quotes",
        action="store_true",
        help="Include quotes when --data all is used. Quotes are excluded by default.",
    )


def normalize_date_args(args: argparse.Namespace) -> None:
    if args.date:
        args.start_date = args.date
        args.end_date = args.date
    if not args.start_date:
        raise SystemExit("Please pass --date or --start-date/--end-date")
    if not args.end_date:
        args.end_date = args.start_date


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Unified Massive data downloader",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  python massive_data_downloader.py catalog\n"
            "  python massive_data_downloader.py flatfiles --asset all --data all --date 2026-06-18\n"
            "  python massive_data_downloader.py rest --asset all --exclude-assets options --data day,minute --date 2026-06-18 --from-flatfiles\n"
            "  python massive_data_downloader.py rest --asset stocks,crypto --data day,minute --date 2026-06-18 --from-flatfiles --max-tickers 10\n"
        ),
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("catalog", help="Print supported categories and data types")

    flatfiles = subparsers.add_parser(
        "flatfiles",
        help="Download bulk Flat Files and extract to CSV",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    add_common_download_args(flatfiles)
    flatfiles.add_argument(
        "--keep-gz",
        action="store_true",
        help="Keep downloaded .csv.gz next to extracted CSV",
    )

    rest = subparsers.add_parser(
        "rest",
        help="Download REST datasets to CSV",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    add_common_download_args(rest)
    rest.add_argument("--tickers", help="Explicit comma-separated tickers, e.g. AAPL,MSFT or X:BTCUSD")
    rest.add_argument(
        "--all-tickers",
        action="store_true",
        help="Discover active tickers from REST reference endpoints. Can be slow.",
    )
    rest.add_argument(
        "--from-flatfiles",
        action="store_true",
        help="Read ticker universe from local Flat Files. Automatically enables --combined-output.",
    )
    rest.add_argument(
        "--combined-output",
        action="store_true",
        help="Write one CSV per asset/data/date under rest_combined instead of one file per ticker.",
    )
    rest.add_argument(
        "--keep-parts",
        action="store_true",
        help="Keep temporary per-ticker part files in downloads/unified/_parts.",
    )
    rest.add_argument(
        "--max-tickers",
        type=int,
        help="Limit ticker count for testing, regardless of ticker source.",
    )
    rest.add_argument(
        "--max-pages",
        type=int,
        help="Limit paginated trades/quotes pages per ticker. For testing only.",
    )
    rest.add_argument(
        "--allow-options-from-flatfiles",
        action="store_true",
        help=(
            "Allow full options REST combined downloads from contract-level Flat File tickers. "
            "Danger: this can create hundreds of thousands of requests."
        ),
    )

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if args.command == "catalog":
        return print_catalog()

    normalize_date_args(args)
    if args.command == "flatfiles":
        return run_flatfiles(args)
    if args.command == "rest":
        if args.from_flatfiles and not args.combined_output:
            args.combined_output = True
        if args.combined_output:
            return run_rest_combined(args)
        return run_rest(args)
    parser.error("unknown command")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
