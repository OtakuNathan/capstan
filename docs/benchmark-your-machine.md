# Benchmark your machine

From the checkout root, one command fetches locked dependencies, configures an
optimized build, runs the full test suite, then measures validated native TCP
requests:

```sh
python3 scripts/bench_machine.py
```

The script supports Linux and macOS and needs Python 3.12+, CMake 3.20+, Git,
a C++14 compiler, pkg-config, GIO and libuv development files. It does not install
system packages or change CPU power policy. Build parallelism defaults to two;
use `--jobs 4` on a machine with enough memory.

## Apple Silicon / M1 Max

Install Xcode Command Line Tools if they are missing (`xcode-select --install`),
and use a native arm64 terminal and Homebrew installation. Install the prerequisites
once with [Homebrew](https://formulae.brew.sh/formula/glib):

```sh
brew install cmake pkgconf glib libuv python
python3 --version  # Must be 3.12 or newer.
python3 scripts/bench_machine.py --jobs 4
```

If your PATH selects Apple's older Python, invoke Homebrew's interpreter:

```sh
"$(brew --prefix)/bin/python3" scripts/bench_machine.py --jobs 4
```

The default compiler on macOS is `clang++` (normally Apple Clang). Homebrew GIO
and libuv prefixes are discovered with `brew --prefix`, following
[Homebrew's external-build guidance](https://docs.brew.sh/How-to-Build-Software-Outside-Homebrew-with-Homebrew-keg-only-Dependencies).
ARM64 native tuning uses `-mcpu=native`; CMake checks compiler support and LTO
availability. The script rejects a Python process running under Rosetta.

The script adds Homebrew's executable directories to its own subprocess PATH
before checking tools, and accepts either `pkg-config` or `pkgconf`. It also checks
the installed `pkgconf` formula when it is not linked into Homebrew's `bin`.
The chosen executable is passed explicitly to CMake. If neither command is
installed, run `brew install pkgconf`; no global shell configuration is changed.

macOS entropy uses `getentropy`, one of Apple's documented
[system random sources](https://support.apple.com/guide/security/seca0c73a75b/web),
in chunks of at most 256 bytes. Linux retains `getrandom`. Both platforms use
the same GIO descriptor-source transport and explicit request-completion path.

## Linux

On Debian/Ubuntu, install prerequisites once if needed:

```sh
sudo apt-get install build-essential cmake git python3 pkg-config libglib2.0-dev libuv1-dev
python3 scripts/bench_machine.py
```

The Python package provided by your distribution must be at least 3.12. The
default compiler is `c++`; `--compiler clang++` selects Clang. On Linux, if Clang
and `ld.lld` are present, the script selects lld for the LTO link.

## Quick check and full measurement

```sh
# Full build and tests, followed by tiny validation batches.
python3 scripts/bench_machine.py --quick

# Full measurement with an explicit, fresh result directory.
python3 scripts/bench_machine.py --output work/m1-max-run1
```

Each full run uses the current optimized implementation: Release/O3, LTO, native
CPU tuning, strict no-exceptions, FF fast runner, UV worker, inline version and
request tracing off. The Dataset is 64 MiB and the history budget is 65,536 per
fresh Host. The two workloads match the final-configuration batch sizes in the
[3700X report](../bench/concurrency/latest/README.md):

| Workload | Connections | Measured calls per batch | Warmup | Rounds |
|---|---|---:|---:|---:|
| version | 1, 2, 4, 8, 16, 32, 48 | 50,000 | 1,000 | 3 |
| summary | 1, 8, 32 | 512 | 8 | 3 |

The quick profile instead uses a 1 MiB Dataset, one round, two concurrency levels
and small call counts. It validates the workflow; its numbers are not comparable
performance results. Neither profile depends on Linux `perf` or `/proc` for
request pacing or success checks.

## Results

By default, each run creates a fresh `work/machine-bench/<UTC timestamp>-<id>/`:

- `SUMMARY.md`: throughput median/min/max and pooled p50/p95/p99.
- `machine.json`: CPU/OS/compiler/library details, source revision, fingerprints,
  profile, command results and overall success.
- `compile_commands.json` and `CMakeCache.txt`: actual build configuration.
- `tests.log`: full CTest result; failures stop the benchmark.
- `version/report.json`, `summary/report.json`, and per-batch `samples.csv`:
  validated measurements, source/binary fingerprints and drained Host accounting.

The default build directory is reused for subsequent runs; `--build-dir` selects
another one. Existing result directories must be empty, so earlier results are
not silently overwritten. Logs remain available if a stage fails.

Connect power, keep the machine awake and avoid concurrent heavy workloads.
Record the existing power mode with your results. A comparison between the
3700X/Linux/GCC and M1 Max/macOS/Apple Clang includes the OS TCP stack, compiler
and library differences as well as the CPU. These remain finite, single-object,
closed-loop experiments, with one outstanding RPC per connection; they do not
establish indefinite-service capacity or open-loop latency.
