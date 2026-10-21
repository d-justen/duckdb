# PRF compression and validation

PRF compression uses bounded, temporary intervals to avoid repeatedly scanning
large clustered bitmaps. Homogeneous 128-word blocks can be skipped during
collection. A small deterministic sample selects the collection algorithm;
compression decisions still use exact counts and the existing FPR and coverage
limits. The final bitmap/direct-range layout and the lookup algorithms are
unchanged. The existing `enable_prefix_range_filter_compression` setting controls
the optimization.

There are at most 65,536 intervals per collection. Serial reduction holds at most
two interval buffers (2 MiB of interval payload). Parallel collection divides
that budget among workers, merges adjacent boundary runs deterministically, and
materializes the final bitmap in disjoint word ranges. An uneven distribution
can exhaust one worker's budget even when the combined run count would fit;
this safely selects bitmap reduction. Collection never mutates the source.
The fallback preserves the large homogeneous-block shortcut and subdivides
mixed blocks into eight destination words.

Focused correctness tests live in
`test/optimizer/prefix_range_filter_compression.cpp`. They compare materialized
bitmap words against independent dyadic reduction, exercise overflow, tail bits,
parallel boundaries, and exhaustive membership, and retain the existing numeric,
string, range, and compression-policy tests. SQL coverage includes one- and
eight-thread joins that materialize a compressed bitmap.

## Comparing construction and probing

`scripts/prepare_prf_comparison.py` and `tools/prf_compression_diagnostics.cpp`
compare an older revision and the working implementation in one executable.
The generator exposes private fields only in generated diagnostic copies, so
there is no production introspection API. The comparison checks every final
bitmap byte, direct ranges, shift, and membership counts. Each construction is
timed with one or eight workers; parallel runs reuse a worker pool rather than
creating operating-system threads for each compression pass.

Probe measurements distinguish:

* the first public lookup after construction;
* the median of nine subsequent public lookups;
* one shared lookup function over each original bitmap allocation;
* the same shared function after copying each bitmap into the same buffer.

The last two measurements are only defined for bitmap mode and report zero for
direct ranges. The same-buffer control isolates contents from allocation and
lookup code placement. It does not substitute for SQL latency measurements.

Example for a Linux build with the core_functions and jemalloc extensions:

```sh
python3 scripts/prepare_prf_comparison.py --baseline 4895e6f2c0 --output /tmp/prf-comparison
clang++ -std=c++17 -O3 -DNDEBUG \
  -Isrc/include -Ithird_party/fmt/include -I/tmp/prf-comparison \
  tools/prf_compression_diagnostics.cpp -o /tmp/prf-comparison/compare \
  build/release/src/libduckdb_static.a \
  build/release/extension/libduckdb_generated_extension_loader.a \
  build/release/extension/core_functions/libcore_functions_extension.a \
  build/release/extension/jemalloc/libjemalloc_extension.a \
  build/release/src/libduckdb_static.a -ldl -pthread
/tmp/prf-comparison/compare 11 /tmp/prf-comparison/results.csv
```

Use the same compiler, optimization flags, headers, and linked extensions as the
DuckDB build. The fixture uses one million build keys, one million probes, a
100-million-value domain, and shuffled cluster order. Baseline/candidate order
is counterbalanced: use an odd total number of rounds, drop round zero, and
report medians across the remaining even number of rounds. This avoids assigning
one algorithm more of the expensive first allocations. Keep first-pass and
warmed results separate. Benchmark SQL joins as well before drawing conclusions
about end-to-end performance.

## Diva on ARM64

Linux ARM64 builds include portable Diva targets and, by default, `_sve2`
variants compiled with SVE2 bit permutation only in the Diva adapter and its
dependencies. PRF, Grafite, and benchmark-driver compilation flags remain equal.
Run an `_sve2` executable only after checking host support for SVE2 bit permutation;
the ordinary executable is the fallback. macOS can validate Diva through a Linux
container. x86 keeps its existing BMI2 selection path.

`diva_bit_tests` and `range_filter_adapter_tests` cover bit operations, bulk-load
boundaries, and point/range correctness. Run their `_sve2` counterparts on a
supporting machine. The vendored provenance file records the portability changes
and the fix for an empty final bulk-load block at `1024*k+1` keys.
