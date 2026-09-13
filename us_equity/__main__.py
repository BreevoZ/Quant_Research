"""Run with python -m us_equity (after editable install, from any directory)."""

import argparse
from datetime import datetime, timezone
from pathlib import Path

from qr import paths


def main() -> int:
    parser = argparse.ArgumentParser(description="US equities: minute baseline and offline demo")
    commands = parser.add_subparsers(dest="command", required=True)
    demo_parser = commands.add_parser("demo", help="Generate synthetic data and run three session checks")
    demo_parser.add_argument("--output", type=Path, help="New directory; default us_research/demo_<UTC time>")
    runner = commands.add_parser("run", help="Process locally downloaded Massive minute CSV/CSV.gz")
    runner.add_argument("--config", type=Path, default=paths.REPO_ROOT / "us_equity/experiments/minute_baseline.toml")
    runner.add_argument("--input", type=Path, help="Minute dataset directory; default QR_US_MINUTE / qr.toml")
    runner.add_argument("--output", type=Path, help="New run directory; default us_research/run_<UTC time>")
    args = parser.parse_args()
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    try:
        if args.command == "demo":
            from .demo import demo
            output = args.output or paths.get("us_research", f"demo_{stamp}")
            for report in demo(output):
                print(f"Synthetic report: {report.resolve()}")
        else:
            from .config import load_experiment
            from .pipeline import run
            experiment = load_experiment(args.config)
            output = args.output or paths.get("us_research", f"run_{stamp}")
            report = run(experiment, args.input or paths.get("us_minute"), output)
            print(f"Research report: {report.resolve()}")
    except (ValueError, FileNotFoundError, FileExistsError) as exc:
        parser.exit(2, f"Error: {exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
