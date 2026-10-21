#!/usr/bin/env python3
"""Generate isolated baseline/candidate PRF classes for the diagnostic driver.

The generated classes retain their implementation but live in separate namespaces.
Private fields are exposed only in these generated test copies, allowing byte-level
comparison without adding a production inspection API. Compile
tools/prf_compression_diagnostics.cpp with the output directory on the include path
and link against a matching DuckDB build. See tools/PRF_COMPRESSION.md.
"""

import argparse
from pathlib import Path
import subprocess


def prepare(source: str, namespace: str) -> str:
    marker = "template <typename T>\nbool ComputeSpan("
    if source.count(marker) != 1 or source.count("namespace duckdb {") != 1:
        raise ValueError("PRF implementation shape changed; review the diagnostic extraction")
    source = source.split(marker)[0]
    source = source.replace("namespace duckdb {", f"namespace duckdb {{ namespace {namespace} {{", 1)
    return source.replace("private:", "public:") + "\n} }\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, help="Git revision before the compression changes")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    path = "src/planner/filter/table_filter_prefix_range_function.cpp"
    baseline = subprocess.check_output(["git", "show", f"{args.baseline}:{path}"], cwd=root, text=True)
    args.output.mkdir(parents=True, exist_ok=True)
    for name, source in [("baseline", baseline), ("candidate", (root / path).read_text())]:
        (args.output / f"prf_{name}.inc").write_text(prepare(source, name))


if __name__ == "__main__":
    main()
