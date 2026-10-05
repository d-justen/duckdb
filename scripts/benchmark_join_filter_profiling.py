#!/usr/bin/env python3
"""Compare runtime filter preparation without changing the execution strategy.

Example:
  python3 scripts/benchmark_join_filter_profiling.py \
      --library build/release/src/libduckdb.dylib \
      --reference-library /path/to/baseline/libduckdb.dylib \
      --repetitions 25 --output /tmp/join-filter-profile.json

Uses only the standard library and DuckDB's public C API. Each library runs in a
separate persistent process. Engine calls are timed inside that process, excluding
IPC, result extraction and startup. Profiled runs use EXPLAIN ANALYZE; unprofiled
runs execute the same SELECT with profiling disabled. Both include planning.

The JSON join_filter metrics measure finalization (including hash-table work),
not filter-only time. Paired differences from the 'none' configuration estimate
incremental filter build overhead. Compression/analysis are reported separately;
Bloom fallback remains in the build measurement. Negative differences are kept.
Detailed profiling is opt-in via --detailed. Its worker times are not elapsed time.

The old PRF build_phase_elapsed_seconds and compression_phase_elapsed_seconds
fields are replaced by join_filter's consistently scoped elapsed metrics. PRF
worker/probe fields now require SET profiling_mode='detailed'. External-join
profiles cover initial finalization only; this harness rejects external joins.
Durations accumulate per operator across actual builds (finalize_count). Summing
operators that run concurrently does not give query wall time.
"""

import argparse
import ctypes
import json
import math
import multiprocessing
from pathlib import Path
import random
import statistics
import time


class Result(ctypes.Structure):
    # Public duckdb_result ABI; deprecated fields must remain in the structure.
    _fields_ = [(name, ctypes.c_uint64) for name in ('columns', 'rows', 'changed')] + [
        (name, ctypes.c_void_p) for name in ('column_data', 'error', 'internal_data')
    ]


class Database:
    def __init__(self, library):
        self.lib = ctypes.CDLL(library)
        ptr = ctypes.c_void_p
        result_ptr = ctypes.POINTER(Result)
        signatures = {
            'duckdb_open': ([ctypes.c_char_p, ctypes.POINTER(ptr)], ctypes.c_int),
            'duckdb_connect': ([ptr, ctypes.POINTER(ptr)], ctypes.c_int),
            'duckdb_close': ([ctypes.POINTER(ptr)], None),
            'duckdb_disconnect': ([ctypes.POINTER(ptr)], None),
            'duckdb_query': ([ptr, ctypes.c_char_p, result_ptr], ctypes.c_int),
            'duckdb_result_error': ([result_ptr], ctypes.c_char_p),
            'duckdb_value_varchar': ([result_ptr, ctypes.c_uint64, ctypes.c_uint64], ptr),
            'duckdb_free': ([ptr], None),
            'duckdb_destroy_result': ([result_ptr], None),
        }
        for name, (args, result) in signatures.items():
            function = getattr(self.lib, name)
            function.argtypes, function.restype = args, result
        self.db, self.connection = ptr(), ptr()
        if self.lib.duckdb_open(None, ctypes.byref(self.db)):
            raise RuntimeError('duckdb_open failed')
        if self.lib.duckdb_connect(self.db, ctypes.byref(self.connection)):
            self.lib.duckdb_close(ctypes.byref(self.db))
            raise RuntimeError('duckdb_connect failed')

    def query(self, sql, column=None):
        result = Result()
        try:
            start = time.perf_counter_ns()
            status = self.lib.duckdb_query(self.connection, sql.encode(), ctypes.byref(result))
            seconds = (time.perf_counter_ns() - start) * 1e-9
            if status:
                raise RuntimeError(self.lib.duckdb_result_error(ctypes.byref(result)).decode())
            value = None
            if column is not None:
                text = self.lib.duckdb_value_varchar(ctypes.byref(result), column, 0)
                if not text:
                    raise RuntimeError('Expected a non-NULL result')
                try:
                    value = ctypes.string_at(text).decode()
                finally:
                    self.lib.duckdb_free(text)
            return seconds, value
        finally:
            self.lib.duckdb_destroy_result(ctypes.byref(result))

    def close(self):
        self.lib.duckdb_disconnect(ctypes.byref(self.connection))
        self.lib.duckdb_close(ctypes.byref(self.db))


def setup_sql(workload, rows):
    if workload == 'small':
        rows = 5000
    if workload in ('small', 'exact'):
        keys = f'SELECT (i*2)::BIGINT k FROM range({rows}) t(i)'
    elif workload == 'clustered':
        keys = (
            f'SELECT i::BIGINT k FROM range({rows // 2}) t(i) UNION ALL '
            f'SELECT (i+90000000)::BIGINT k FROM range({rows - rows // 2}) t(i)'
        )
    else:
        keys = f'SELECT (i*2000000000)::BIGINT k FROM range({rows}) t(i)'
    return (
        f'DROP TABLE IF EXISTS probe; DROP TABLE IF EXISTS build; '
        f'CREATE TABLE build AS {keys}; '
        f'CREATE TABLE probe AS SELECT k FROM build UNION ALL SELECT k+1 FROM build;'
    ), rows


def shape(node):
    info = node.get('extra_info', {})
    return (
        node.get('operator_type'),
        info.get('Table'),
        info.get('Join Type'),
        info.get('Conditions'),
        tuple(shape(child) for child in node.get('children', [])),
    )


def find_join(node):
    result = [node] if node.get('operator_type') == 'HASH_JOIN' else []
    for child in node.get('children', []):
        result.extend(find_join(child))
    return result


def worker(library, pipe):
    db = None
    try:
        db = Database(library)
        db.query(
            "SET disabled_optimizers='join_order,build_side_probe_side'; "
            'SET enable_perfect_hash_join_filter_pushdown=false; '
            'SET enable_join_min_max_filter_pushdown=false;'
        )
        pipe.send({'ready': True})
        while True:
            request = pipe.recv()
            if request is None:
                break
            if request['action'] == 'setup':
                sql, _ = setup_sql(request['workload'], request['rows'])
                db.query('SET threads=1; ' + sql)
                # Clustered probe keys k+1 can also match. Obtain the unfiltered reference result once.
                db.query('SET enable_prefix_range_filter=false; SET enable_join_bloom_filter_pushdown=false;')
                _, expected = db.query(QUERY, 0)
                pipe.send({'expected': expected})
                continue
            mode = request['mode']
            db.query(
                f"SET threads={request['threads']}; "
                f"SET enable_prefix_range_filter={str(mode in ('compressed', 'uncompressed')).lower()}; "
                f"SET enable_prefix_range_filter_compression={str(mode == 'compressed').lower()}; "
                f"SET enable_join_bloom_filter_pushdown={str(mode != 'none').lower()}; "
                f"SET profiling_mode='{'detailed' if request['detailed'] else 'standard'}'; "
                'PRAGMA disable_profiling;'
            )
            profiled = request['profiled']
            sql = ('EXPLAIN (ANALYZE, FORMAT JSON) ' if profiled else '') + QUERY
            seconds, value = db.query(sql, 1 if profiled else 0)
            response = {'query_seconds': seconds}
            if profiled:
                profile = json.loads(value)
                joins = find_join(profile)
                if len(joins) != 1:
                    raise RuntimeError('Expected exactly one physical hash join')
                join = joins[0]
                metrics = join.get('join_filter')
                if metrics and (metrics['perfect_hash_join_count'] or metrics['external_finalize_count']):
                    raise RuntimeError('Expected an ordinary in-memory hash join')
                response.update(
                    metrics=metrics,
                    plan=shape(join),
                    build_rows=join['children'][1]['operator_cardinality'],
                    cardinality=str(join['operator_cardinality']),
                    prf=join.get('prefix_range_filter', {}),
                )
            else:
                response['cardinality'] = value
            pipe.send(response)
    except BaseException as exc:
        pipe.send({'error': str(exc)})
    finally:
        if db:
            db.close()
        pipe.close()


# A second, non-equality condition prevents perfect hash joins even on small inputs.
# perfect_ht_threshold alone does not disable their runtime selection on this branch.
QUERY = 'SELECT count(*) FROM probe p JOIN build b ON p.k=b.k AND p.k%7>=b.k%7;'


class Worker:
    def __init__(self, library):
        ctx = multiprocessing.get_context('spawn')
        self.pipe, child_pipe = ctx.Pipe()
        self.process = ctx.Process(target=worker, args=(library, child_pipe))
        self.process.start()
        child_pipe.close()
        try:
            self.receive()
        except BaseException:
            self.close()
            raise

    def receive(self):
        if not self.pipe.poll(180):
            raise RuntimeError('Benchmark worker timed out')
        response = self.pipe.recv()
        if 'error' in response:
            raise RuntimeError(response['error'])
        return response

    def request(self, **request):
        self.pipe.send(request)
        return self.receive()

    def close(self):
        if self.process.is_alive():
            try:
                self.pipe.send(None)
            except (BrokenPipeError, EOFError):
                pass
        self.process.join(5)
        if self.process.is_alive():
            self.process.terminate()
            self.process.join()
        self.pipe.close()


def percentile(values, fraction):
    values = sorted(values)
    position = (len(values) - 1) * fraction
    lower, upper = math.floor(position), math.ceil(position)
    return values[lower] + (values[upper] - values[lower]) * (position - lower)


def describe(values):
    return dict(
        median=statistics.median(values), p10=percentile(values, 0.1), p90=percentile(values, 0.9), samples=values
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--library', required=True, type=Path)
    parser.add_argument('--reference-library', type=Path)
    parser.add_argument('--rows', type=int, default=1300000)
    parser.add_argument('--threads', type=int, nargs='+', default=[1, 4])
    parser.add_argument(
        '--workloads',
        choices=['small', 'exact', 'clustered', 'sparse'],
        nargs='+',
        default=['small', 'exact', 'clustered', 'sparse'],
    )
    parser.add_argument('--repetitions', type=int, default=15)
    parser.add_argument('--warmups', type=int, default=2)
    parser.add_argument('--seed', type=int, default=42)
    parser.add_argument('--detailed', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.rows < 1000 or args.repetitions < 3 or args.warmups < 1 or min(args.threads) < 1:
        parser.error('Require rows>=1000, repetitions>=3, warmups>=1 and positive thread counts')
    libraries = {'candidate': str(args.library.resolve(strict=True))}
    if args.reference_library:
        libraries['reference'] = str(args.reference_library.resolve(strict=True))
    workers = {}
    samples = {}
    rng = random.Random(args.seed)
    try:
        for label, library in libraries.items():
            workers[label] = Worker(library)
        for workload in args.workloads:
            expected = {
                label: w.request(action='setup', workload=workload, rows=args.rows)['expected']
                for label, w in workers.items()
            }
            if len(set(expected.values())) != 1:
                raise RuntimeError('Libraries disagree on the reference query result')
            for threads in args.threads:
                plan = None
                for iteration in range(-args.warmups, args.repetitions):
                    jobs = [
                        (label, profiled, mode)
                        for label in workers
                        for profiled in (False, True)
                        for mode in ('none', 'bloom', 'compressed', 'uncompressed')
                    ]
                    rng.shuffle(jobs)
                    for label, profiled, mode in jobs:
                        result = workers[label].request(
                            action='query', threads=threads, mode=mode, profiled=profiled, detailed=args.detailed
                        )
                        if result['cardinality'] != expected[label]:
                            raise RuntimeError(f'Wrong query result: {label}, {workload}, {mode}')
                        if profiled:
                            signature = (result['plan'], result['build_rows'])
                            if plan is not None and signature != plan:
                                raise RuntimeError('Physical join plan or build workload changed across configurations')
                            plan = signature
                            if label == 'candidate':
                                metrics = result['metrics']
                                if metrics is None:
                                    raise RuntimeError('Candidate library does not provide join_filter elapsed metrics')
                                prf_expected = mode in ('compressed', 'uncompressed')
                                bloom_expected = mode == 'bloom' or bool(
                                    result['prf'].get('bloom_fallback_selected', 0)
                                )
                                if (
                                    metrics['finalize_count'] != 1
                                    or metrics['prf_build_count'] != int(prf_expected)
                                    or metrics['bloom_build_count'] != int(bloom_expected)
                                ):
                                    raise RuntimeError('Requested filter configuration was not executed')
                        if iteration >= 0:
                            key = (label, workload, threads, profiled, mode)
                            samples.setdefault(key, []).append(result)
    finally:
        for w in workers.values():
            w.close()
    report = {
        'libraries': libraries,
        'rows': args.rows,
        'seed': args.seed,
        'detailed': args.detailed,
        'repetitions': args.repetitions,
        'measurements': [],
        'comparisons': [],
    }
    for key, runs in samples.items():
        label, workload, threads, profiled, mode = key
        entry = dict(
            library=label,
            workload=workload,
            threads=threads,
            profiled=profiled,
            mode=mode,
            query_seconds=describe([r['query_seconds'] for r in runs]),
        )
        if profiled and runs[0]['metrics']:
            for metric in runs[0]['metrics']:
                entry[metric] = describe([r['metrics'][metric] for r in runs])
            baseline = samples[(label, workload, threads, True, 'none')]
            entry['filter_build_overhead_seconds'] = describe(
                [
                    r['metrics']['join_finalize_build_elapsed_seconds']
                    - b['metrics']['join_finalize_build_elapsed_seconds']
                    for r, b in zip(runs, baseline)
                ]
            )
            entry['bloom_fallback_selected'] = runs[0]['prf'].get('bloom_fallback_selected', 0)
        report['measurements'].append(entry)
        if label == 'candidate' and 'reference' in workers:
            reference = samples[('reference', workload, threads, profiled, mode)]
            ratios = [r['query_seconds'] / b['query_seconds'] for r, b in zip(runs, reference)]
            bootstrap = [statistics.median(rng.choices(ratios, k=len(ratios))) for _ in range(1000)]
            report['comparisons'].append(
                dict(
                    workload=workload,
                    threads=threads,
                    profiled=profiled,
                    mode=mode,
                    candidate_over_reference=statistics.median(ratios),
                    bootstrap_95_percent_interval=[percentile(bootstrap, 0.025), percentile(bootstrap, 0.975)],
                )
            )
    output = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.write_text(output)
        print(f'Wrote {len(report["measurements"])} measurements to {args.output}')
    else:
        print(output, end='')


if __name__ == '__main__':
    main()
