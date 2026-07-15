#include "duckdb.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace duckdb;

#if defined(DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE)
#include "grafite_benchmark_adapter.hpp"
#endif
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_DIVA)
#include "diva_benchmark_adapter.hpp"
#endif

namespace {
using Clock = std::chrono::steady_clock;

struct Config {
	uint64_t domain_size = 10000000;
	idx_t total_keys = 100000;
	idx_t min_clusters = 2;
	idx_t max_clusters = 128;
	idx_t queries_per_cluster_count = 100000;
	idx_t repetitions = 5;
	uint64_t seed = 42;
	double grafite_bits_per_key = 12.0;
	double diva_bits_per_key = 16.0;
	string output_path;
};

struct QueryRange {
	uint64_t lower;
	uint64_t upper;
	uint64_t width;
};

struct ClusterLayout {
	vector<uint64_t> keys;
	vector<QueryRange> gaps;
	uint64_t min_gap_width = 0;
	uint64_t max_gap_width = 0;
	double mean_gap_width = 0;
};

bool StartsWith(const string &arg, const string &prefix) {
	return arg.rfind(prefix, 0) == 0;
}

bool IsPowerOfTwo(idx_t value) {
	return value > 0 && (value & (value - 1)) == 0;
}

uint64_t ParseUnsigned(const string &arg, const string &name) {
	try {
		size_t pos;
		auto value = stoull(arg, &pos);
		if (pos != arg.size()) {
			throw InvalidInputException("Invalid numeric value for %s: %s", name, arg);
		}
		return value;
	} catch (const std::exception &) {
		throw InvalidInputException("Invalid numeric value for %s: %s", name, arg);
	}
}

Config ParseArguments(int argc, char *argv[]) {
	Config config;
	for (int i = 1; i < argc; i++) {
		const string arg(argv[i]);
		if (StartsWith(arg, "--domain-size=")) {
			config.domain_size = ParseUnsigned(arg.substr(14), "domain-size");
		} else if (StartsWith(arg, "--total-keys=")) {
			config.total_keys = UnsafeNumericCast<idx_t>(ParseUnsigned(arg.substr(13), "total-keys"));
		} else if (StartsWith(arg, "--min-clusters=")) {
			config.min_clusters = UnsafeNumericCast<idx_t>(ParseUnsigned(arg.substr(15), "min-clusters"));
		} else if (StartsWith(arg, "--max-clusters=")) {
			config.max_clusters = UnsafeNumericCast<idx_t>(ParseUnsigned(arg.substr(15), "max-clusters"));
		} else if (StartsWith(arg, "--queries-per-cluster-count=")) {
			config.queries_per_cluster_count =
			    UnsafeNumericCast<idx_t>(ParseUnsigned(arg.substr(28), "queries-per-cluster-count"));
		} else if (StartsWith(arg, "--repetitions=")) {
			config.repetitions = UnsafeNumericCast<idx_t>(ParseUnsigned(arg.substr(14), "repetitions"));
		} else if (StartsWith(arg, "--seed=")) {
			config.seed = ParseUnsigned(arg.substr(7), "seed");
		} else if (StartsWith(arg, "--grafite-bpk=")) {
			config.grafite_bits_per_key = stod(arg.substr(14));
		} else if (StartsWith(arg, "--diva-bpk=")) {
			config.diva_bits_per_key = stod(arg.substr(11));
		} else if (StartsWith(arg, "--output=")) {
			config.output_path = arg.substr(9);
		} else if (arg == "--help") {
			std::cout << "Usage: range_benchmark [options]\n"
			          << "  --domain-size=N --total-keys=N\n"
			          << "  --min-clusters=N --max-clusters=N (powers of two)\n"
			          << "  --queries-per-cluster-count=N --repetitions=N --seed=N\n"
			          << "  --grafite-bpk=N --diva-bpk=N --output=PATH\n";
			std::exit(0);
		} else {
			throw InvalidInputException("Unknown argument: %s", arg);
		}
	}
	if (config.domain_size == 0 || config.total_keys == 0 || config.total_keys >= config.domain_size ||
	    config.min_clusters < 2 || config.max_clusters < config.min_clusters ||
	    !IsPowerOfTwo(config.min_clusters) || !IsPowerOfTwo(config.max_clusters) ||
	    config.max_clusters > config.total_keys || config.max_clusters - 1 > config.domain_size - config.total_keys ||
	    config.queries_per_cluster_count == 0 || config.repetitions == 0) {
		throw InvalidInputException("invalid benchmark configuration");
	}
	if (config.grafite_bits_per_key <= 2.0 || config.diva_bits_per_key <= 1.0) {
		throw InvalidInputException("invalid bits-per-key configuration");
	}
	return config;
}

vector<idx_t> GenerateClusterCounts(const Config &config) {
	vector<idx_t> result;
	for (idx_t count = config.min_clusters; count <= config.max_clusters;) {
		result.push_back(count);
		if (count > config.max_clusters / 2) {
			break;
		}
		count <<= 1;
	}
	return result;
}

ClusterLayout GenerateClusterLayout(uint64_t domain_size, idx_t total_keys, idx_t cluster_count) {
	const idx_t cluster_length = total_keys / cluster_count;
	const idx_t cluster_remainder = total_keys % cluster_count;
	const uint64_t free_values = domain_size - total_keys;
	const idx_t gap_count = cluster_count - 1;
	const uint64_t base_gap = free_values / gap_count;
	const uint64_t gap_remainder = free_values % gap_count;

	ClusterLayout layout;
	layout.keys.reserve(total_keys);
	layout.gaps.reserve(gap_count);
	uint64_t cursor = 0;
	uint64_t total_gap_width = 0;
	for (idx_t cluster_idx = 0; cluster_idx < cluster_count; cluster_idx++) {
		const idx_t length = cluster_length + (cluster_idx < cluster_remainder ? 1 : 0);
		for (idx_t i = 0; i < length; i++) {
			layout.keys.push_back(cursor++);
		}
		if (cluster_idx + 1 < cluster_count) {
			const uint64_t width = base_gap + (cluster_idx < gap_remainder ? 1 : 0);
			layout.gaps.push_back({cursor, cursor + width - 1, width});
			cursor += width;
			total_gap_width += width;
		}
	}
	D_ASSERT(layout.keys.size() == total_keys);
	D_ASSERT(layout.keys.front() == 0 && layout.keys.back() == domain_size - 1);
	D_ASSERT(!layout.gaps.empty());
	layout.min_gap_width = layout.gaps.back().width;
	layout.max_gap_width = layout.gaps.front().width;
	layout.mean_gap_width = static_cast<double>(total_gap_width) / static_cast<double>(layout.gaps.size());
	return layout;
}

void FillVector(Vector &result, const vector<uint64_t> &values) {
	auto data = FlatVector::GetDataMutable<uint64_t>(result);
	for (idx_t i = 0; i < values.size(); i++) {
		data[i] = values[i];
	}
	FlatVector::SetSize(result, values.size());
}

unique_ptr<PrefixRangeFilter> BuildPRF(ClientContext &context, const vector<uint64_t> &keys, bool compress) {
	auto filter = PrefixRangeFilter::CreatePrefixRangeFilter(LogicalType::UBIGINT);
	PrefixRangeFilter::Sizing sizing;
	if (!PrefixRangeFilter::TryComputeSizing(Value::UBIGINT(keys.front()), Value::UBIGINT(keys.back()), keys.size(),
	                                         sizing)) {
		throw InternalException("Failed to compute PRF sizing");
	}
	filter->Initialize(context, keys.size(), Value::UBIGINT(keys.front()), Value::UBIGINT(keys.back()), sizing);
	Vector vector(LogicalType::UBIGINT, keys.size());
	FillVector(vector, keys);
	auto state = filter->InitializeBuildState(context);
	filter->InsertKeys(vector, keys.size(), *state);
	filter->MergeBuildState(*state);
	if (compress) {
		filter->Compress(context, 0.001);
	}
	return filter;
}

idx_t PRFBytes(const PrefixRangeFilter::CompressionInfo &info) {
	return info.mode == CompressionMode::DIRECT_RANGES ? info.range_count * 2 * sizeof(uint64_t)
	                                                  : ((info.logical_bucket_count + 63) / 64) * sizeof(uint64_t);
}

template <class FUNC>
uint64_t TimeQueries(const vector<QueryRange> &gaps, idx_t count, idx_t &possibly_overlapping, FUNC &&query) {
	possibly_overlapping = 0;
	for (const auto &gap : gaps) {
		possibly_overlapping += query(gap) ? 1 : 0;
	}
	possibly_overlapping = 0;
	const auto start = Clock::now();
	for (idx_t i = 0; i < count; i++) {
		possibly_overlapping += query(gaps[i % gaps.size()]) ? 1 : 0;
	}
	return UnsafeNumericCast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

void WriteResult(std::ostream &out, idx_t repetition, idx_t cluster_count, const ClusterLayout &layout,
                 const char *summary, const char *mode, idx_t query_count, idx_t bytes, const char *compression_mode,
                 idx_t shift, idx_t range_count, idx_t active_buckets, uint64_t ns, idx_t possibly_overlapping) {
	const auto throughput = ns == 0 ? 0.0 : static_cast<double>(query_count) * 1e9 / static_cast<double>(ns);
	out << repetition << ',' << cluster_count << ',' << layout.gaps.size() << ",all," << layout.min_gap_width << ','
	    << layout.max_gap_width << ',' << std::fixed << std::setprecision(3) << layout.mean_gap_width << ',' << summary
	    << ',' << mode << ',' << query_count << ',' << ns << ',' << throughput << ',' << possibly_overlapping << ','
	    << (possibly_overlapping == 0 ? "true" : "false") << ',' << bytes << ',' << compression_mode << ',' << shift
	    << ',' << range_count << ',' << active_buckets << '\n';
}

} // namespace

int main(int argc, char *argv[]) {
	try {
		const auto config = ParseArguments(argc, argv);
		std::ofstream file_out;
		std::ostream *out = &std::cout;
		if (!config.output_path.empty()) {
			file_out.open(config.output_path);
			if (!file_out.good()) {
				throw IOException("Failed to open output file %s", config.output_path);
			}
			out = &file_out;
		}
		*out << "repetition,cluster_count,gap_count,gap_selection,min_range_width,max_range_width,mean_range_width,"
		        "summary,mode,query_count,total_query_time_ns,queries_per_sec,possibly_overlapping,correctly_reports_no_overlap,"
		        "summary_bytes,compression_mode,shift,range_count,active_buckets\n";

		DuckDB db(nullptr);
		Connection con(db);
		for (idx_t rep = 0; rep < config.repetitions; rep++) {
			auto cluster_counts = GenerateClusterCounts(config);
			std::mt19937_64 rng(config.seed + rep);
			std::shuffle(cluster_counts.begin(), cluster_counts.end(), rng);
			for (const auto cluster_count : cluster_counts) {
				const auto layout = GenerateClusterLayout(config.domain_size, config.total_keys, cluster_count);
				auto prf = BuildPRF(*con.context, layout.keys, false);
				auto compressed_prf = BuildPRF(*con.context, layout.keys, true);

				idx_t matches;
				auto info = prf->GetCompressionInfo();
				auto ns = TimeQueries(layout.gaps, config.queries_per_cluster_count, matches, [&](const QueryRange &gap) {
					return prf->LookupRange(Value::UBIGINT(gap.lower), Value::UBIGINT(gap.upper)) !=
					       FilterPropagateResult::FILTER_ALWAYS_FALSE;
				});
				WriteResult(*out, rep, cluster_count, layout, "prf", "uncompressed", config.queries_per_cluster_count,
				            PRFBytes(info), "bitmap", info.shift, info.range_count, info.active_buckets, ns, matches);

				info = compressed_prf->GetCompressionInfo();
				ns = TimeQueries(layout.gaps, config.queries_per_cluster_count, matches, [&](const QueryRange &gap) {
					return compressed_prf->LookupRange(Value::UBIGINT(gap.lower), Value::UBIGINT(gap.upper)) !=
					       FilterPropagateResult::FILTER_ALWAYS_FALSE;
				});
				WriteResult(*out, rep, cluster_count, layout, "prf", "compressed", config.queries_per_cluster_count,
				            PRFBytes(info), info.mode == CompressionMode::DIRECT_RANGES ? "direct_ranges" : "bitmap",
				            info.shift, info.range_count, info.active_buckets, ns, matches);

#if defined(DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE)
				GrafiteBenchmarkFilter grafite;
				grafite.Build(layout.keys, config.grafite_bits_per_key);
				ns = TimeQueries(layout.gaps, config.queries_per_cluster_count, matches,
				                 [&](const QueryRange &gap) { return grafite.RangeProbe(gap.lower, gap.upper); });
				WriteResult(*out, rep, cluster_count, layout, "grafite", "na", config.queries_per_cluster_count,
				            UnsafeNumericCast<idx_t>(grafite.SizeBytes()), "na", 0, 0, 0, ns, matches);
#endif
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_DIVA)
				DivaBenchmarkFilter diva;
				diva.Build(layout.keys, config.diva_bits_per_key);
				ns = TimeQueries(layout.gaps, config.queries_per_cluster_count, matches,
				                 [&](const QueryRange &gap) { return diva.RangeProbe(gap.lower, gap.upper); });
				WriteResult(*out, rep, cluster_count, layout, "diva", "na", config.queries_per_cluster_count,
				            UnsafeNumericCast<idx_t>(diva.SizeBytes()), "na", 0, 0, 0, ns, matches);
#endif
			}
		}
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << ex.what() << '\n';
		return 1;
	}
}
