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

enum class LayoutMode : uint8_t { TWO_CLUSTERS, SCATTERED_MIDDLE_GAP };

struct Config {
	uint64_t domain_size = 100000000;
	idx_t total_keys = 100000;
	uint64_t min_range_width = 1;
	uint64_t max_range_width = 0; // largest valid power of two
	idx_t queries_per_width = 100000;
	idx_t repetitions = 5;
	double grafite_bits_per_key = 12.0;
	double diva_bits_per_key = 16.0;
	LayoutMode layout_mode = LayoutMode::TWO_CLUSTERS;
	string output_path;
};

struct QueryRange { uint64_t lower; uint64_t upper; uint64_t width; };
struct KeyLayout { std::vector<uint64_t> keys; QueryRange query; };

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
		else if (StartsWith(arg, "--layout=")) {
			const auto value = arg.substr(9);
			if (value == "two-clusters") config.layout_mode = LayoutMode::TWO_CLUSTERS;
			else if (value == "scattered-middle-gap") config.layout_mode = LayoutMode::SCATTERED_MIDDLE_GAP;
			else throw InvalidInputException("Unknown layout: %s", value);
		}
		else if (StartsWith(arg, "--output=")) config.output_path = arg.substr(9);
		else if (arg == "--help") {
			std::cout << "Usage: range_benchmark [options]\n"
			          << "  --domain-size=N --total-keys=N\n"
			          << "  --min-range-width=N --max-range-width=N (powers of two; 0 selects largest valid)\n"
			          << "  --layout=two-clusters|scattered-middle-gap\n"
			          << "  --queries-per-width=N --repetitions=N --grafite-bpk=N --diva-bpk=N --output=PATH\n";
			std::exit(0);
		} else throw InvalidInputException("Unknown argument: %s", arg);
	}
	if (config.domain_size == 0 || config.total_keys == 0 || config.total_keys >= config.domain_size ||
	    !IsPowerOfTwo(config.min_range_width) || (config.max_range_width != 0 && !IsPowerOfTwo(config.max_range_width)) ||
	    config.queries_per_width == 0 || config.repetitions == 0) {
		throw InvalidInputException("invalid benchmark configuration");
	}
	if (config.layout_mode == LayoutMode::SCATTERED_MIDDLE_GAP && config.total_keys < 3) {
		throw InvalidInputException("scattered-middle-gap requires at least three keys");
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

std::vector<uint64_t> GenerateRangeWidths(const Config &config) {
	const auto gap_width = config.domain_size - config.total_keys;
	const auto max_width = config.max_range_width == 0 ? LargestPowerOfTwo(gap_width) : config.max_range_width;
	if (max_width > gap_width || config.min_range_width > max_width) throw InvalidInputException("range widths do not fit middle gap");
	std::vector<uint64_t> result;
	for (uint64_t width = config.min_range_width; width <= max_width;) {
		result.push_back(width);
		if (width > max_width / 2) break;
		width <<= 1;
	}
	return result;
}

KeyLayout GenerateTwoClusterLayout(uint64_t domain_size, idx_t total_keys, uint64_t width) {
	auto keys = GenerateTwoClusterKeys(domain_size, total_keys);
	const auto first_cluster = UnsafeNumericCast<uint64_t>((total_keys + 1) / 2);
	const auto gap_width = domain_size - total_keys;
	const auto lower = first_cluster + (gap_width - width) / 2;
	return {std::move(keys), {lower, lower + width - 1, width}};
}

KeyLayout GenerateScatteredMiddleGapLayout(uint64_t domain_size, idx_t total_keys, uint64_t width) {
	const auto remaining_free_values = domain_size - total_keys - width;
	const auto normal_gap_count = total_keys - 2;
	const auto normal_gap = remaining_free_values / normal_gap_count;
	const auto normal_gap_remainder = remaining_free_values % normal_gap_count;
	const auto center_gap_after_key = (total_keys - 1) / 2;

	std::vector<uint64_t> keys;
	keys.reserve(total_keys);
	keys.push_back(0);
	idx_t normal_gap_idx = 0;
	QueryRange query {0, 0, width};
	for (idx_t key_idx = 0; key_idx + 1 < total_keys; key_idx++) {
		uint64_t gap;
		if (key_idx == center_gap_after_key) {
			gap = width;
			query.lower = keys.back() + 1;
			query.upper = query.lower + width - 1;
		} else {
			gap = normal_gap + (normal_gap_idx < normal_gap_remainder ? 1 : 0);
			normal_gap_idx++;
		}
		keys.push_back(keys.back() + gap + 1);
	}
	D_ASSERT(keys.back() == domain_size - 1);
	return {std::move(keys), query};
}

KeyLayout GenerateLayout(const Config &config, uint64_t width) {
	if (config.layout_mode == LayoutMode::TWO_CLUSTERS) {
		return GenerateTwoClusterLayout(config.domain_size, config.total_keys, width);
	}
	return GenerateScatteredMiddleGapLayout(config.domain_size, config.total_keys, width);
}

const char *LayoutName(LayoutMode mode) {
	return mode == LayoutMode::TWO_CLUSTERS ? "two_clusters" : "scattered_middle_gap";
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
void WriteResult(std::ostream &out, const char *layout, const char *summary, const char *mode, QueryRange range, idx_t count, idx_t bytes,
                 FUNC &&query) {
	idx_t possibly_overlapping = 0;
	const auto start = Clock::now();
	for (idx_t i = 0; i < count; i++) possibly_overlapping += query() ? 1 : 0;
	const auto ns = UnsafeNumericCast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
	const auto throughput = ns == 0 ? 0.0 : static_cast<double>(count) * 1e9 / static_cast<double>(ns);
	out << layout << ',' << summary << ',' << mode << ',' << range.width << ',' << range.lower << ',' << range.upper << ',' << ns << ','
	    << std::fixed << std::setprecision(3) << throughput << ',' << possibly_overlapping << ','
	    << (possibly_overlapping == 0 ? "true" : "false") << ',' << bytes << '\n';
}

} // namespace

int main(int argc, char *argv[]) {
	try {
		const auto config = ParseArguments(argc, argv);
		const auto widths = GenerateRangeWidths(config);
		std::ofstream file_out;
		std::ostream *out = &std::cout;
		if (!config.output_path.empty()) { file_out.open(config.output_path); if (!file_out.good()) throw IOException("Failed to open output file %s", config.output_path); out = &file_out; }
		*out << "layout,summary,mode,range_width,lower,upper,total_query_time_ns,queries_per_sec,possibly_overlapping,correctly_reports_no_overlap,summary_bytes\n";
		DuckDB db(nullptr);
		Connection con(db);
		for (idx_t rep = 0; rep < config.repetitions; rep++) {
			for (const auto width : widths) {
				const auto layout = GenerateLayout(config, width);
				auto prf = BuildPRF(*con.context, layout.keys, false);
				auto compressed_prf = BuildPRF(*con.context, layout.keys, true);
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE)
			GrafiteBenchmarkFilter grafite;
			grafite.Build(layout.keys, config.grafite_bits_per_key);
#endif
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_DIVA)
			DivaBenchmarkFilter diva;
			diva.Build(layout.keys, config.diva_bits_per_key);
#endif
				WriteResult(*out, LayoutName(config.layout_mode), "prf", "uncompressed", layout.query, config.queries_per_width,
				            ((prf->GetCompressionInfo().logical_bucket_count + 63) / 64) * sizeof(uint64_t),
				            [&] { return prf->LookupRange(Value::UBIGINT(layout.query.lower), Value::UBIGINT(layout.query.upper)) != FilterPropagateResult::FILTER_ALWAYS_FALSE; });
				WriteResult(*out, LayoutName(config.layout_mode), "prf", "compressed", layout.query, config.queries_per_width,
				            compressed_prf->GetCompressionInfo().mode == CompressionMode::DIRECT_RANGES ? compressed_prf->GetCompressionInfo().range_count * 2 * sizeof(uint64_t) : ((compressed_prf->GetCompressionInfo().logical_bucket_count + 63) / 64) * sizeof(uint64_t),
				            [&] { return compressed_prf->LookupRange(Value::UBIGINT(layout.query.lower), Value::UBIGINT(layout.query.upper)) != FilterPropagateResult::FILTER_ALWAYS_FALSE; });
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE)
				WriteResult(*out, LayoutName(config.layout_mode), "grafite", "na", layout.query, config.queries_per_width, UnsafeNumericCast<idx_t>(grafite.SizeBytes()), [&] { return grafite.RangeProbe(layout.query.lower, layout.query.upper); });
#endif
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_DIVA)
				WriteResult(*out, LayoutName(config.layout_mode), "diva", "na", layout.query, config.queries_per_width, UnsafeNumericCast<idx_t>(diva.SizeBytes()), [&] { return diva.RangeProbe(layout.query.lower, layout.query.upper); });
#endif
			}
		}
		return 0;
	} catch (const std::exception &ex) { std::cerr << ex.what() << '\n'; return 1; }
}
