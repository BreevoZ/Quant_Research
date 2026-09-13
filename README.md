# Quant Research · 量化研究

**Two research tracks, one shared toolkit. / 两条研究主线，一套共享工具。**

Research infrastructure for mainland China A-shares and US equities. The A-share track covers
order-level reconstruction, L3 factors and execution simulation. The US track starts with Massive
minute bars, causal features, delayed labels and reproducible diagnostics.

本仓库研究中国 A 股与美股。A 股侧覆盖逐订单重建、L3 因子和成交模拟；
美股侧从 Massive 分钟数据、因果特征、延迟标签和可复现评估开始。

## Start here · 从这里开始

| Entry / 入口 | Contents / 内容 |
|---|---|
| **[A-shares · A 股](ashare/README.md)** | Reconstruction, L3 research, execution simulation / 重建、L3 研究、成交模拟 |
| **[US equities · 美股](us_equity/README.md)** | Minute baseline, offline demo, real-data ingestion / 分钟基线、离线 demo、真实数据接入 |
| [Setup · 环境配置](docs/SETUP.md) | Existing `mx` conda environment, dependencies and checks / 现有 `mx` 环境、依赖与验证 |
| [Architecture · 仓库结构](docs/ARCHITECTURE.md) | Module boundaries, shared code and paths / 模块边界、共享代码与路径 |
| [Migration · 目录迁移](docs/MIGRATION.md) | Old → new paths and local-machine notes / 新旧路径对照与本机迁移说明 |
| [Validation · 验证记录](docs/VALIDATION.md) | Tested environment, checks and limits / 已验证环境、检查与边界 |
| [Pre-push · 提交前检查](docs/PRE_PUSH.md) | Credential guard and staged-change review / 密钥检查与暂存内容核对 |

The **three main README entry points** are this overview, A-shares and US equities. Component READMEs
remain as technical references. Main entry points and setup guides are bilingual; older research
reports are identified as Chinese-language historical material.

**三个主 README** 分别承担全仓导航、A 股说明、美股说明。子项目 README 保留技术细节。
主入口和环境说明提供中英双语；旧研究报告保留中文原文并标注语言与历史性质。

## Layout · 目录

```text
Quant_Research/
├── README.md                 # Repository overview / 全仓入口
├── ashare/                   # A-share infrastructure and research / A 股
│   ├── README.md
│   ├── tltoflow/             # TL → unified event flow
│   ├── datacheck/            # Reference LOB reconstruction
│   ├── auction/              # Auction snapshots
│   ├── build_tick3s/         # Three-second panels
│   ├── vorder_sim/           # Queue and execution simulation
│   ├── l3_factor/            # Main L3 research
│   └── factor/               # Earlier A-share factors; legacy
├── us_equity/                # US research package / 美股
│   ├── README.md
│   ├── experiments/
│   └── tests/
├── data_sources/massive/      # Shared multi-asset downloader / 多资产下载器
├── qr/                       # Shared paths and binary readers / 共享工具
├── third_party/              # Build dependencies and stubs / 构建依赖
├── docs/                     # Setup, architecture, migration / 公共说明
├── scripts/                  # Repository checks / 仓库检查
├── archive/                  # Legacy crypto experiments / 历史实验
├── data/                     # Local data and generated files; ignored / 本地数据
├── environment.yml           # Conda environment recipe
├── pyproject.toml
└── qr.example.toml           # Local-path configuration template
```

Massive supports multiple asset classes, so its downloader is shared. Historical crypto experiments
are kept outside the two active market tracks.

Massive 支持多类资产，因此下载器放在共享数据源层；早期 crypto 实验单独归档。

## Quick start with conda · 使用 conda 开始

From the repository root / 在仓库根目录执行：

```bash
conda activate mx
python -m pip install -e ".[us,massive]"
python -m us_equity demo
python -m unittest discover -s us_equity/tests -v
python scripts/check_repository.py
python scripts/check_push.py
```

The demo requires no market-data credentials. Synthetic results test the pipeline, not investment
performance. New-machine instructions are in [Setup](docs/SETUP.md).

demo 无需行情密钥，合成数据结果仅验证管道。新机器的环境安装见[配置说明](docs/SETUP.md)。

For C++ builds and data-dependent research, use the [A-share entry point](ashare/README.md).
Both tracks share `qr.toml` and `QR_*` path overrides. Raw data, secrets and new US run outputs are
ignored; selected historical A-share result snapshots remain versioned.

A 股 C++ 构建与真实数据研究命令见 [A 股入口](ashare/README.md)。两条主线共用
`qr.toml` 和 `QR_*` 配置；原始行情、密钥和新美股运行产物不入库，已有部分 A 股结果快照保留版本记录。
