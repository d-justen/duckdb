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
	uint64_t domain_size = 100000000;
	idx_t total_keys = 100000;
	uint64_t min_range_width = 1;
	uint64_t max_range_width = 0; // largest valid power of two
	idx_t queries_per_width = 100000;
	idx_t repetitions = 5;
	double grafite_bits_per_key = 12.0;
	double diva_bits_per_key = 16.0;
	string output_path;
};

struct QueryRange { uint64_t lower; uint64_t upper; uint64_t width; };

bool StartsWith(const string &arg, const string &prefix) { return arg.rfind(prefix, 0) == 0; }
bool IsPowerOfTwo(uint64_t value) { return value > 0 && (value & (value - 1)) == 0; }
uint64_t ParseUnsigned(const string &arg, const string &name) {
	try {
		size_t pos;
		auto value = stoull(arg, &pos);
		if (pos != arg.size()) throw InvalidInputException("Invalid numeric value for %s: %s", name, arg);
		return value;
	} catch (const std::exception &) { throw InvalidInputException("Invalid numeric value for %s: %s", name, arg); }
}

Config ParseArguments(int argc, char *argv[]) {
	Config config;
	for (int i = 1; i < argc; i++) {
		const string arg(argv[i]);
		if (StartsWith(arg, "--domain-size=")) config.domain_size = ParseUnsigned(arg.substr(14), "domain-size");
		else if (StartsWith(arg, "--total-keys=")) config.total_keys = UnsafeNumericCast<idx_t>(ParseUnsigned(arg.substr(13), "total-keys"));
		else if (StartsWith(arg, "--min-range-width=")) config.min_range_width = ParseUnsigned(arg.substr(18), "min-range-width");
		else if (StartsWith(arg, "--max-range-width=")) config.max_range_width = ParseUnsigned(arg.substr(18), "max-range-width");
		else if (StartsWith(arg, "--queries-per-width=")) config.queries_per_width = UnsafeNumericCast<idx_t>(ParseUnsigned(arg.substr(20), "queries-per-width"));
		else if (StartsWith(arg, "--repetitions=")) config.repetitions = UnsafeNumericCast<idx_t>(ParseUnsigned(arg.substr(14), "repetitions"));
		else if (StartsWith(arg, "--grafite-bpk=")) config.grafite_bits_per_key = stod(arg.substr(14));
		else if (StartsWith(arg, "--diva-bpk=")) config.diva_bits_per_key = stod(arg.substr(11));
		else if (StartsWith(arg, "--output=")) config.output_path = arg.substr(9);
		else if (arg == "--help") {
			std::cout << "Usage: range_benchmark [options]\n"
			          << "  --domain-size=N --total-keys=N\n"
			          << "  --min-range-width=N --max-range-width=N (powers of two; 0 selects largest valid)\n"
			          << "  --queries-per-width=N --repetitions=N --grafite-bpk=N --diva-bpk=N --output=PATH\n";
			std::exit(0);
		} else throw InvalidInputException("Unknown argument: %s", arg);
	}
	if (config.domain_size == 0 || config.total_keys == 0 || config.total_keys >= config.domain_size ||
	    !IsPowerOfTwo(config.min_range_width) || (config.max_range_width != 0 && !IsPowerOfTwo(config.max_range_width)) ||
	    config.queries_per_width == 0 || config.repetitions == 0) {
		throw InvalidInputException("invalid benchmark configuration");
	}
	return config;
}

std::vector<uint64_t> GenerateTwoClusterKeys(uint64_t domain_size, idx_t total_keys) {
	const idx_t first_cluster = (total_keys + 1) / 2;
	const idx_t second_cluster = total_keys / 2;
	std::vector<uint64_t> keys;
	keys.reserve(total_keys);
	for (idx_t i = 0; i < first_cluster; i++) keys.push_back(i);
	for (idx_t i = 0; i < second_cluster; i++) keys.push_back(domain_size - second_cluster + i);
	return keys;
}

uint64_t LargestPowerOfTwo(uint64_t value) {
	uint64_t result = 1;
	while (result <= value / 2) result <<= 1;
	return result;
}

std::vector<QueryRange> GenerateQueries(const Config &config) {
	const auto first_cluster = UnsafeNumericCast<uint64_t>((config.total_keys + 1) / 2);
	const auto second_cluster = UnsafeNumericCast<uint64_t>(config.total_keys / 2);
	const auto gap_width = config.domain_size - first_cluster - second_cluster;
	const auto max_width = config.max_range_width == 0 ? LargestPowerOfTwo(gap_width) : config.max_range_width;
	if (max_width > gap_width || config.min_range_width > max_width) throw InvalidInputException("range widths do not fit middle gap");
	std::vector<QueryRange> result;
	for (uint64_t width = config.min_range_width; width <= max_width;) {
		const auto lower = first_cluster + (gap_width - width) / 2;
		result.push_back({lower, lower + width - 1, width});
		if (width > max_width / 2) break;
		width <<= 1;
	}
	return result;
}

void FillVector(Vector &vector, const std::vector<uint64_t> &values) {
	auto data = FlatVector::GetDataMutable<uint64_t>(vector);
	for (idx_t i = 0; i < values.size(); i++) data[i] = values[i];
	FlatVector::SetSize(vector, values.size());
}

unique_ptr<PrefixRangeFilter> BuildPRF(ClientContext &context, const std::vector<uint64_t> &keys, bool compress) {
	auto filter = PrefixRangeFilter::CreatePrefixRangeFilter(LogicalType::UBIGINT);
	PrefixRangeFilter::Sizing sizing;
	if (!PrefixRangeFilter::TryComputeSizing(Value::UBIGINT(keys.front()), Value::UBIGINT(keys.back()), keys.size(), sizing))
		throw InternalException("Failed to compute PRF sizing");
	filter->Initialize(context, keys.size(), Value::UBIGINT(keys.front()), Value::UBIGINT(keys.back()), sizing);
	Vector vector(LogicalType::UBIGINT, keys.size());
	FillVector(vector, keys);
	auto state = filter->InitializeBuildState(context);
	filter->InsertKeys(vector, keys.size(), *state);
	filter->MergeBuildState(*state);
	if (compress) filter->Compress(context, 0.001);
	return filter;
}

template <class FUNC>
void WriteResult(std::ostream &out, const char *summary, const char *mode, QueryRange range, idx_t count, idx_t bytes,
                 FUNC &&query) {
	idx_t possibly_overlapping = 0;
	const auto start = Clock::now();
	for (idx_t i = 0; i < count; i++) possibly_overlapping += query() ? 1 : 0;
	const auto ns = UnsafeNumericCast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
	const auto throughput = ns == 0 ? 0.0 : static_cast<double>(count) * 1e9 / static_cast<double>(ns);
	out << summary << ',' << mode << ',' << range.width << ',' << range.lower << ',' << range.upper << ',' << ns << ','
	    << std::fixed << std::setprecision(3) << throughput << ',' << possibly_overlapping << ','
	    << (possibly_overlapping == 0 ? "true" : "false") << ',' << bytes << '\n';
}

} // namespace

int main(int argc, char *argv[]) {
	try {
		const auto config = ParseArguments(argc, argv);
		const auto keys = GenerateTwoClusterKeys(config.domain_size, config.total_keys);
		const auto queries = GenerateQueries(config);
		std::ofstream file_out;
		std::ostream *out = &std::cout;
		if (!config.output_path.empty()) { file_out.open(config.output_path); if (!file_out.good()) throw IOException("Failed to open output file %s", config.output_path); out = &file_out; }
		*out << "summary,mode,range_width,lower,upper,total_query_time_ns,queries_per_sec,possibly_overlapping,correctly_reports_no_overlap,summary_bytes\n";
		DuckDB db(nullptr);
		Connection con(db);
		for (idx_t rep = 0; rep < config.repetitions; rep++) {
			auto prf = BuildPRF(*con.context, keys, false);
			auto compressed_prf = BuildPRF(*con.context, keys, true);
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE)
			GrafiteBenchmarkFilter grafite;
			grafite.Build(keys, config.grafite_bits_per_key);
#endif
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_DIVA)
			DivaBenchmarkFilter diva;
			diva.Build(keys, config.diva_bits_per_key);
#endif
			for (const auto &range : queries) {
				WriteResult(*out, "prf", "uncompressed", range, config.queries_per_width,
				            ((prf->GetCompressionInfo().logical_bucket_count + 63) / 64) * sizeof(uint64_t),
				            [&] { return prf->LookupRange(Value::UBIGINT(range.lower), Value::UBIGINT(range.upper)) != FilterPropagateResult::FILTER_ALWAYS_FALSE; });
				WriteResult(*out, "prf", "compressed", range, config.queries_per_width,
				            compressed_prf->GetCompressionInfo().mode == CompressionMode::DIRECT_RANGES ? compressed_prf->GetCompressionInfo().range_count * 2 * sizeof(uint64_t) : ((compressed_prf->GetCompressionInfo().logical_bucket_count + 63) / 64) * sizeof(uint64_t),
				            [&] { return compressed_prf->LookupRange(Value::UBIGINT(range.lower), Value::UBIGINT(range.upper)) != FilterPropagateResult::FILTER_ALWAYS_FALSE; });
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE)
				WriteResult(*out, "grafite", "na", range, config.queries_per_width, UnsafeNumericCast<idx_t>(grafite.SizeBytes()), [&] { return grafite.RangeProbe(range.lower, range.upper); });
#endif
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_DIVA)
				WriteResult(*out, "diva", "na", range, config.queries_per_width, UnsafeNumericCast<idx_t>(diva.SizeBytes()), [&] { return diva.RangeProbe(range.lower, range.upper); });
#endif
			}
		}
		return 0;
	} catch (const std::exception &ex) { std::cerr << ex.what() << '\n'; return 1; }
}
