#include "duckdb.hpp"
#include "duckdb/common/chrono.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
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

struct BenchmarkConfig {
	uint64_t domain_size = 10000000;
	idx_t total_keys = 100000;
	idx_t probe_count = 1000000;
	idx_t min_clusters = 2;
	idx_t max_clusters = 100;
	idx_t cluster_step = 1;
	idx_t repetitions = 5;
	uint64_t seed = 42;
	double grafite_bits_per_key = 12.0;
	double diva_bits_per_key = 16.0;
	string output_path;
};

struct ClusterLayout {
	vector<uint64_t> keys;
	idx_t cluster_length = 0;
	idx_t cluster_length_remainder = 0;
	idx_t base_gap = 0;
	idx_t gap_remainder = 0;
};

struct BloomRunResult {
	uint64_t build_core_ns = 0;
	uint64_t probe_ns = 0;
	idx_t matched_count = 0;
	idx_t true_positive_count = 0;
	idx_t false_positive_count = 0;
	idx_t bytes = 0;
};

struct PRFRunResult {
	uint64_t build_core_ns = 0;
	uint64_t compress_ns = 0;
	uint64_t analyze_ns = 0;
	uint64_t probe_ns = 0;
	idx_t matched_count = 0;
	idx_t true_positive_count = 0;
	idx_t false_positive_count = 0;
	idx_t bytes = 0;
	CompressionMode mode = CompressionMode::BITMAP;
	idx_t shift = 0;
	idx_t range_count = 0;
	double estimated_fpr = 0;
	idx_t active_buckets = 0;
};

struct DivaRunResult {
	uint64_t build_ns = 0;
	uint64_t probe_ns = 0;
	idx_t matched_count = 0;
	idx_t true_positive_count = 0;
	idx_t false_positive_count = 0;
	idx_t bytes = 0;
};

struct GrafiteRunResult {
	uint64_t build_ns = 0;
	uint64_t probe_ns = 0;
	idx_t matched_count = 0;
	idx_t true_positive_count = 0;
	idx_t false_positive_count = 0;
	idx_t bytes = 0;
};

struct MembershipInfo {
	vector<uint8_t> flags;
	idx_t positives = 0;
};

struct RepetitionContext {
	vector<uint64_t> probes;
	vector<idx_t> cluster_counts;
};

struct ResultRow {
	idx_t cluster_count;
	string line;
};

template <class FUNC>
uint64_t TimeNs(FUNC &&func) {
	const auto start = Clock::now();
	func();
	const auto end = Clock::now();
	return UnsafeNumericCast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

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

BenchmarkConfig ParseArguments(int argc, char *argv[]) {
	BenchmarkConfig config;
	for (int i = 1; i < argc; i++) {
		const string arg(argv[i]);
		if (StartsWith(arg, "--domain-size=")) {
			config.domain_size = ParseUnsigned(arg.substr(14), "domain-size");
		} else if (StartsWith(arg, "--total-keys=")) {
			config.total_keys = UnsafeNumericCast<idx_t>(ParseUnsigned(arg.substr(13), "total-keys"));
		} else if (StartsWith(arg, "--probe-count=")) {
			config.probe_count = UnsafeNumericCast<idx_t>(ParseUnsigned(arg.substr(14), "probe-count"));
		} else if (StartsWith(arg, "--min-clusters=")) {
			config.min_clusters = UnsafeNumericCast<idx_t>(ParseUnsigned(arg.substr(15), "min-clusters"));
		} else if (StartsWith(arg, "--max-clusters=")) {
			config.max_clusters = UnsafeNumericCast<idx_t>(ParseUnsigned(arg.substr(15), "max-clusters"));
		} else if (StartsWith(arg, "--cluster-step=")) {
			config.cluster_step = UnsafeNumericCast<idx_t>(ParseUnsigned(arg.substr(15), "cluster-step"));
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
			std::cout << "Usage: filter_benchmark [options]\n"
			          << "  --domain-size=N\n"
			          << "  --total-keys=N\n"
			          << "  --probe-count=N\n"
			          << "  --min-clusters=N\n"
			          << "  --max-clusters=N\n"
			          << "  --cluster-step=N (accepted for compatibility, ignored; cluster counts use powers of two)\n"
			          << "  --repetitions=N\n"
			          << "  --seed=N\n"
			          << "  --grafite-bpk=N\n"
			          << "  --diva-bpk=N\n"
			          << "  --output=PATH\n";
			std::exit(0);
		} else {
			throw InvalidInputException("Unknown argument: %s", arg);
		}
	}
	if (config.domain_size == 0) {
		throw InvalidInputException("domain-size must be > 0");
	}
	if (config.total_keys == 0 || config.total_keys >= config.domain_size) {
		throw InvalidInputException("total-keys must be > 0 and < domain-size");
	}
	if (config.probe_count == 0) {
		throw InvalidInputException("probe-count must be > 0");
	}
	if (config.min_clusters == 0 || config.max_clusters < config.min_clusters || config.cluster_step == 0) {
		throw InvalidInputException("invalid cluster range");
	}
	if (!IsPowerOfTwo(config.min_clusters) || !IsPowerOfTwo(config.max_clusters)) {
		throw InvalidInputException("min-clusters and max-clusters must be powers of two");
	}
	if (config.repetitions == 0) {
		throw InvalidInputException("repetitions must be > 0");
	}
	if (config.grafite_bits_per_key <= 2.0) {
		throw InvalidInputException("grafite-bpk must be > 2");
	}
	if (config.diva_bits_per_key <= 1.0) {
		throw InvalidInputException("diva-bpk must be > 1");
	}
	return config;
}

ClusterLayout GenerateClusteredKeys(uint64_t domain_size, idx_t total_keys, idx_t cluster_count) {
	if (cluster_count == 0 || total_keys < cluster_count) {
		throw InvalidInputException("cluster-count must be > 0 and <= total-keys");
	}
	ClusterLayout layout;
	layout.cluster_length = total_keys / cluster_count;
	layout.cluster_length_remainder = total_keys % cluster_count;

	const auto free_values = UnsafeNumericCast<idx_t>(domain_size - total_keys);
	const auto gap_count = cluster_count > 1 ? cluster_count - 1 : 0;
	layout.base_gap = gap_count > 0 ? free_values / gap_count : 0;
	layout.gap_remainder = gap_count > 0 ? free_values % gap_count : 0;

	layout.keys.reserve(total_keys);
	uint64_t cursor = 0;
	for (idx_t cluster_idx = 0; cluster_idx < cluster_count; cluster_idx++) {
		const idx_t cluster_len = layout.cluster_length + (cluster_idx < layout.cluster_length_remainder ? 1 : 0);
		for (idx_t i = 0; i < cluster_len; i++) {
			layout.keys.push_back(cursor + i);
		}
		cursor += cluster_len;
		if (cluster_idx + 1 < cluster_count) {
			const idx_t gap = layout.base_gap + (cluster_idx < layout.gap_remainder ? 1 : 0);
			cursor += gap;
		}
	}
	D_ASSERT(layout.keys.size() == total_keys);
	D_ASSERT(layout.keys.back() < domain_size);
	return layout;
}

vector<uint64_t> GenerateProbeKeys(uint64_t domain_size, idx_t probe_count, uint64_t seed) {
	vector<uint64_t> probes;
	probes.reserve(probe_count);
	std::mt19937_64 rng(seed);
	std::uniform_int_distribution<uint64_t> dist(0, domain_size - 1);
	for (idx_t i = 0; i < probe_count; i++) {
		probes.push_back(dist(rng));
	}
	return probes;
}

vector<idx_t> GenerateClusterCounts(const BenchmarkConfig &config) {
	vector<idx_t> cluster_counts;
	for (idx_t cluster_count = config.min_clusters; cluster_count <= config.max_clusters; cluster_count <<= 1) {
		cluster_counts.push_back(cluster_count);
		if (cluster_count > NumericLimits<idx_t>::Maximum() / 2) {
			break;
		}
	}
	return cluster_counts;
}

RepetitionContext CreateRepetitionContext(const BenchmarkConfig &config, idx_t rep) {
	RepetitionContext result;
	result.probes = GenerateProbeKeys(config.domain_size, config.probe_count, config.seed + rep);
	result.cluster_counts = GenerateClusterCounts(config);

	std::mt19937_64 rng(config.seed + 0x9E3779B97F4A7C15ULL + rep);
	std::shuffle(result.cluster_counts.begin(), result.cluster_counts.end(), rng);
	return result;
}

MembershipInfo ComputeMembership(uint64_t domain_size, const vector<uint64_t> &build_keys,
                                 const vector<uint64_t> &probe_keys) {
	MembershipInfo info;
	info.flags.resize(probe_keys.size(), 0);
	vector<uint8_t> in_build(domain_size, 0);
	for (auto key : build_keys) {
		in_build[UnsafeNumericCast<idx_t>(key)] = 1;
	}
	for (idx_t i = 0; i < probe_keys.size(); i++) {
		const auto present = in_build[UnsafeNumericCast<idx_t>(probe_keys[i])] != 0;
		info.flags[i] = present ? 1 : 0;
		info.positives += present ? 1 : 0;
	}
	return info;
}

void FillUBigIntVector(Vector &vector, const std::vector<uint64_t> &values) {
	auto data = FlatVector::GetDataMutable<uint64_t>(vector);
	for (idx_t i = 0; i < values.size(); i++) {
		data[i] = values[i];
	}
	FlatVector::SetSize(vector, values.size());
}

double ComputeActualFalsePositiveRate(idx_t false_positive_count, const MembershipInfo &membership, idx_t probe_count) {
	const auto negative_probe_count = probe_count - membership.positives;
	if (negative_probe_count == 0) {
		return 0.0;
	}
	return static_cast<double>(false_positive_count) / static_cast<double>(negative_probe_count);
}

idx_t CountMatchedPositives(const SelectionVector &sel, idx_t match_count, const MembershipInfo &membership) {
	idx_t true_positives = 0;
	for (idx_t i = 0; i < match_count; i++) {
		const auto idx = sel.get_index(i);
		true_positives += membership.flags[idx] != 0;
	}
	return true_positives;
}

idx_t EstimateBloomBytes(idx_t number_of_rows) {
	static constexpr idx_t MIN_NUM_BITS_PER_KEY = 12;
	static constexpr idx_t MIN_NUM_BITS = 512;
	static constexpr idx_t LOG_SECTOR_SIZE = 6;
	static constexpr idx_t MAX_NUM_SECTORS = (1ULL << 26);
	const idx_t min_bits = MaxValue<idx_t>(MIN_NUM_BITS, number_of_rows * MIN_NUM_BITS_PER_KEY);
	const idx_t num_sectors = MinValue<idx_t>(NextPowerOfTwo(min_bits) >> LOG_SECTOR_SIZE, MAX_NUM_SECTORS);
	return num_sectors * sizeof(uint64_t);
}

BloomRunResult RunBloomFilterBenchmark(ClientContext &context, const vector<uint64_t> &build_keys,
                                       const vector<uint64_t> &probe_keys, const MembershipInfo &membership) {
	BloomRunResult result;

	Vector build_vector(LogicalType::UBIGINT, build_keys.size());
	Vector probe_vector(LogicalType::UBIGINT, probe_keys.size());
	FillUBigIntVector(build_vector, build_keys);
	FillUBigIntVector(probe_vector, probe_keys);

	Vector build_hashes(LogicalType::HASH, build_keys.size());
	Vector probe_hashes(LogicalType::HASH, probe_keys.size());

	result.build_core_ns = TimeNs([&]() {
		VectorOperations::Hash(build_vector, build_hashes, build_keys.size());
		build_hashes.Flatten();
		BloomFilter filter;
		filter.Initialize(context, build_keys.size());
		filter.InsertHashes(build_hashes, build_keys.size());
	});

	BloomFilter filter;
	VectorOperations::Hash(build_vector, build_hashes, build_keys.size());
	build_hashes.Flatten();
	filter.Initialize(context, build_keys.size());
	filter.InsertHashes(build_hashes, build_keys.size());

	SelectionVector sel(probe_keys.size());
	result.probe_ns = TimeNs([&]() {
		VectorOperations::Hash(probe_vector, probe_hashes, probe_keys.size());
		probe_hashes.Flatten();
		result.matched_count = filter.LookupHashes(probe_hashes, sel, probe_keys.size());
	});
	result.true_positive_count = CountMatchedPositives(sel, result.matched_count, membership);
	result.false_positive_count = result.matched_count - result.true_positive_count;
	result.bytes = EstimateBloomBytes(build_keys.size());
	return result;
}

PRFRunResult RunPrefixRangeFilterBenchmark(ClientContext &context, const vector<uint64_t> &build_keys,
                                           const vector<uint64_t> &probe_keys, const MembershipInfo &membership,
                                           bool enable_compression) {
	PRFRunResult result;
	auto filter = PrefixRangeFilter::CreatePrefixRangeFilter(LogicalType::UBIGINT);
	PrefixRangeFilter::Sizing sizing;
	const auto min_key = build_keys.front();
	const auto max_key = build_keys.back();
	if (!PrefixRangeFilter::TryComputeSizing(Value::UBIGINT(min_key), Value::UBIGINT(max_key), build_keys.size(),
	                                         sizing)) {
		throw InternalException("Failed to compute PRF sizing");
	}
	filter->Initialize(context, build_keys.size(), Value::UBIGINT(min_key), Value::UBIGINT(max_key), sizing);

	Vector build_vector(LogicalType::UBIGINT, build_keys.size());
	Vector probe_vector(LogicalType::UBIGINT, probe_keys.size());
	FillUBigIntVector(build_vector, build_keys);
	FillUBigIntVector(probe_vector, probe_keys);

	auto build_state = filter->InitializeBuildState(context);
	result.build_core_ns = TimeNs([&]() {
		filter->InsertKeys(build_vector, build_keys.size(), *build_state);
		filter->MergeBuildState(*build_state);
	});

	static constexpr double PRF_FALSE_POSITIVE_RATE_THRESHOLD = 0.001;
	if (enable_compression) {
		result.compress_ns = TimeNs([&]() { filter->Compress(context, PRF_FALSE_POSITIVE_RATE_THRESHOLD); });
	}
	PrefixRangeFilter::Analysis analysis;
	result.analyze_ns = TimeNs([&]() { analysis = filter->Analyze(); });

	SelectionVector sel(probe_keys.size());
	result.probe_ns =
	    TimeNs([&]() { result.matched_count = filter->LookupKeys(probe_vector, sel, probe_keys.size()); });
	result.true_positive_count = CountMatchedPositives(sel, result.matched_count, membership);
	result.false_positive_count = result.matched_count - result.true_positive_count;

	const auto info = filter->GetCompressionInfo();
	result.mode = info.mode;
	result.shift = info.shift;
	result.range_count = info.range_count;
	result.estimated_fpr = info.false_positive_rate;
	result.active_buckets = analysis.active_buckets;
	result.bytes = info.mode == CompressionMode::DIRECT_RANGES
	                   ? UnsafeNumericCast<idx_t>(info.range_count * 2 * sizeof(uint64_t))
	                   : UnsafeNumericCast<idx_t>(((info.logical_bucket_count + 63) / 64) * sizeof(uint64_t));
	return result;
}

#if defined(DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE)
GrafiteRunResult RunGrafiteBenchmark(const std::vector<uint64_t> &build_keys, const std::vector<uint64_t> &probe_keys,
                                     const MembershipInfo &membership, double bits_per_key) {
	GrafiteRunResult result;

	GrafiteBenchmarkFilter filter;
	result.build_ns = TimeNs([&]() { filter.Build(build_keys, bits_per_key); });

	result.probe_ns = TimeNs([&]() {
		for (idx_t i = 0; i < probe_keys.size(); i++) {
			const auto matched = filter.Probe(probe_keys[i]);
			result.matched_count += matched ? 1 : 0;
			result.true_positive_count += matched && membership.flags[i] ? 1 : 0;
		}
	});
	result.false_positive_count = result.matched_count - result.true_positive_count;
	result.bytes = UnsafeNumericCast<idx_t>(filter.SizeBytes());
	return result;
}
#endif

#if defined(DUCKDB_FILTER_BENCHMARK_HAS_DIVA)
DivaRunResult RunDivaBenchmark(const std::vector<uint64_t> &build_keys, const std::vector<uint64_t> &probe_keys,
                               const MembershipInfo &membership, double bits_per_key) {
	DivaRunResult result;

	DivaBenchmarkFilter filter;
	result.build_ns = TimeNs([&]() { filter.Build(build_keys, bits_per_key); });

	result.probe_ns = TimeNs([&]() {
		for (idx_t i = 0; i < probe_keys.size(); i++) {
			const auto matched = filter.Probe(probe_keys[i]);
			result.matched_count += matched ? 1 : 0;
			result.true_positive_count += matched && membership.flags[i] ? 1 : 0;
		}
	});
	result.false_positive_count = result.matched_count - result.true_positive_count;
	result.bytes = UnsafeNumericCast<idx_t>(filter.SizeBytes());
	return result;
}
#endif

void WriteHeader(std::ostream &out) {
	out << "filter,cluster_count,build_time_ns,post_processing_time_ns,summary_bytes,probe_throughput_per_sec,"
	       "false_positives,estimated_fpr,actual_fpr,mode,range_count,shift,active_buckets\n";
}

string FormatBloomRow(const BenchmarkConfig &config, idx_t cluster_count, const MembershipInfo &membership,
                      const BloomRunResult &result) {
	const auto probes_per_sec =
	    result.probe_ns == 0 ? 0.0
	                         : static_cast<double>(config.probe_count) * 1e9 / static_cast<double>(result.probe_ns);
	const auto actual_fpr = ComputeActualFalsePositiveRate(result.false_positive_count, membership, config.probe_count);
	std::ostringstream out;
	out << "bloom," << cluster_count << ',' << result.build_core_ns << ',' << 0 << ',' << result.bytes << ','
	    << std::fixed << std::setprecision(3) << probes_per_sec << ',' << result.false_positive_count << ',' << ""
	    << ',' << std::setprecision(9) << actual_fpr << ',' << "na,0,0,0";
	return out.str();
}

const char *CompressionModeName(CompressionMode mode) {
	switch (mode) {
	case CompressionMode::BITMAP:
		return "bitmap";
	case CompressionMode::DIRECT_RANGES:
		return "direct_ranges";
	default:
		return "unknown";
	}
}

string FormatPRFRow(const char *filter_name, const BenchmarkConfig &config, idx_t cluster_count,
                    const MembershipInfo &membership, const PRFRunResult &result) {
	const auto probes_per_sec =
	    result.probe_ns == 0 ? 0.0
	                         : static_cast<double>(config.probe_count) * 1e9 / static_cast<double>(result.probe_ns);
	const auto post_build_ns = result.compress_ns + result.analyze_ns;
	const auto actual_fpr = ComputeActualFalsePositiveRate(result.false_positive_count, membership, config.probe_count);

	std::ostringstream out;
	out << filter_name << ',' << cluster_count << ',' << result.build_core_ns << ',' << post_build_ns << ','
	    << result.bytes << ',' << std::fixed << std::setprecision(3) << probes_per_sec << ','
	    << result.false_positive_count << ',' << std::setprecision(9) << result.estimated_fpr << ',' << actual_fpr
	    << ',' << CompressionModeName(result.mode) << ',' << result.range_count << ',' << result.shift << ','
	    << result.active_buckets;
	return out.str();
}

#if defined(DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE)
string FormatGrafiteRow(const BenchmarkConfig &config, idx_t cluster_count, const MembershipInfo &membership,
                        const GrafiteRunResult &result) {
	const auto probes_per_sec =
	    result.probe_ns == 0 ? 0.0
	                         : static_cast<double>(config.probe_count) * 1e9 / static_cast<double>(result.probe_ns);
	const auto actual_fpr = ComputeActualFalsePositiveRate(result.false_positive_count, membership, config.probe_count);
	std::ostringstream out;
	out << "grafite," << cluster_count << ',' << result.build_ns << ',' << 0 << ',' << result.bytes << ',' << std::fixed
	    << std::setprecision(3) << probes_per_sec << ',' << result.false_positive_count << ',' << "" << ','
	    << std::setprecision(9) << actual_fpr << ',' << "na,0,0,0";
	return out.str();
}
#endif

#if defined(DUCKDB_FILTER_BENCHMARK_HAS_DIVA)
string FormatDivaRow(const BenchmarkConfig &config, idx_t cluster_count, const MembershipInfo &membership,
                     const DivaRunResult &result) {
	const auto probes_per_sec =
	    result.probe_ns == 0 ? 0.0
	                         : static_cast<double>(config.probe_count) * 1e9 / static_cast<double>(result.probe_ns);
	const auto actual_fpr = ComputeActualFalsePositiveRate(result.false_positive_count, membership, config.probe_count);
	std::ostringstream out;
	out << "diva," << cluster_count << ',' << result.build_ns << ',' << 0 << ',' << result.bytes << ',' << std::fixed
	    << std::setprecision(3) << probes_per_sec << ',' << result.false_positive_count << ',' << "" << ','
	    << std::setprecision(9) << actual_fpr << ',' << "na,0,0,0";
	return out.str();
}
#endif

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

		DuckDB db(nullptr);
		Connection con(db);
		auto &context = *con.context;

		WriteHeader(*out);
		for (idx_t rep = 0; rep < config.repetitions; rep++) {
			const auto repetition = CreateRepetitionContext(config, rep);
			vector<ResultRow> rows;
			rows.reserve(repetition.cluster_counts.size() * 5);
			for (const auto cluster_count : repetition.cluster_counts) {
				const auto layout = GenerateClusteredKeys(config.domain_size, config.total_keys, cluster_count);
				const auto membership = ComputeMembership(config.domain_size, layout.keys, repetition.probes);

				(void)RunBloomFilterBenchmark(context, layout.keys, repetition.probes, membership);
				const auto bloom_result = RunBloomFilterBenchmark(context, layout.keys, repetition.probes, membership);
				rows.push_back({cluster_count, FormatBloomRow(config, cluster_count, membership, bloom_result)});

				(void)RunPrefixRangeFilterBenchmark(context, layout.keys, repetition.probes, membership, false);
				const auto prf_uncompressed_result =
				    RunPrefixRangeFilterBenchmark(context, layout.keys, repetition.probes, membership, false);
				rows.push_back({cluster_count, FormatPRFRow("prf_uncompressed", config, cluster_count, membership,
				                                            prf_uncompressed_result)});

				(void)RunPrefixRangeFilterBenchmark(context, layout.keys, repetition.probes, membership, true);
				const auto prf_result =
				    RunPrefixRangeFilterBenchmark(context, layout.keys, repetition.probes, membership, true);
				rows.push_back({cluster_count, FormatPRFRow("prf", config, cluster_count, membership, prf_result)});

#if defined(DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE)
				(void)RunGrafiteBenchmark(layout.keys, repetition.probes, membership, config.grafite_bits_per_key);
				const auto grafite_result =
				    RunGrafiteBenchmark(layout.keys, repetition.probes, membership, config.grafite_bits_per_key);
				rows.push_back({cluster_count, FormatGrafiteRow(config, cluster_count, membership, grafite_result)});
#endif

#if defined(DUCKDB_FILTER_BENCHMARK_HAS_DIVA)
				(void)RunDivaBenchmark(layout.keys, repetition.probes, membership, config.diva_bits_per_key);
				const auto diva_result =
				    RunDivaBenchmark(layout.keys, repetition.probes, membership, config.diva_bits_per_key);
				rows.push_back({cluster_count, FormatDivaRow(config, cluster_count, membership, diva_result)});
#endif
			}
			std::sort(rows.begin(), rows.end(),
			          [](const ResultRow &lhs, const ResultRow &rhs) { return lhs.cluster_count < rhs.cluster_count; });
			for (const auto &row : rows) {
				*out << row.line << '\n';
			}
		}

		return 0;
	} catch (const std::exception &ex) {
		std::cerr << ex.what() << '\n';
		return 1;
	}
}
