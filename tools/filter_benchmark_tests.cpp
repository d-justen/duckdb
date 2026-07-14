#include "duckdb.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"

#include <iostream>
#include <vector>

using namespace duckdb;

#if defined(DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE)
#include "grafite_benchmark_adapter.hpp"
#endif

#if defined(DUCKDB_FILTER_BENCHMARK_HAS_DIVA)
#include "diva_benchmark_adapter.hpp"
#endif

namespace {

void FillVector(Vector &vector, const std::vector<uint64_t> &values) {
	auto data = FlatVector::GetDataMutable<uint64_t>(vector);
	for (idx_t i = 0; i < values.size(); i++) {
		data[i] = values[i];
	}
	FlatVector::SetSize(vector, values.size());
}

std::vector<uint64_t> GenerateClusteredKeysForTest(uint64_t domain_size, idx_t total_keys, idx_t cluster_count) {
	const idx_t cluster_length = total_keys / cluster_count;
	const idx_t cluster_length_remainder = total_keys % cluster_count;

	const auto free_values = UnsafeNumericCast<idx_t>(domain_size - total_keys);
	const auto gap_count = cluster_count > 1 ? cluster_count - 1 : 0;
	const auto base_gap = gap_count > 0 ? free_values / gap_count : 0;
	const auto gap_remainder = gap_count > 0 ? free_values % gap_count : 0;

	std::vector<uint64_t> keys;
	keys.reserve(total_keys);
	uint64_t cursor = 0;
	for (idx_t cluster_idx = 0; cluster_idx < cluster_count; cluster_idx++) {
		const idx_t cluster_len = cluster_length + (cluster_idx < cluster_length_remainder ? 1 : 0);
		for (idx_t i = 0; i < cluster_len; i++) {
			keys.push_back(cursor + i);
		}
		cursor += cluster_len;
		if (cluster_idx + 1 < cluster_count) {
			const idx_t gap = base_gap + (cluster_idx < gap_remainder ? 1 : 0);
			cursor += gap;
		}
	}
	return keys;
}

bool TestBloom(DuckDB &db) {
	Connection con(db);
	auto &context = *con.context;

	std::vector<uint64_t> build_keys = {0, 1, 10, 11, 20, 21};
	std::vector<uint64_t> probes = {0, 2, 10, 12, 20, 22};

	Vector build_vec(LogicalType::UBIGINT, build_keys.size());
	Vector probe_vec(LogicalType::UBIGINT, probes.size());
	FillVector(build_vec, build_keys);
	FillVector(probe_vec, probes);

	Vector build_hashes(LogicalType::HASH, build_keys.size());
	Vector probe_hashes(LogicalType::HASH, probes.size());
	VectorOperations::Hash(build_vec, build_hashes, build_keys.size());
	VectorOperations::Hash(probe_vec, probe_hashes, probes.size());
	build_hashes.Flatten();
	probe_hashes.Flatten();

	BloomFilter bloom;
	bloom.Initialize(context, build_keys.size());
	bloom.InsertHashes(build_hashes, build_keys.size());

	SelectionVector result_sel(probes.size());
	const auto match_count = bloom.LookupHashes(probe_hashes, result_sel, probes.size());
	if (match_count < 3) {
		std::cerr << "Bloom sanity check failed: expected at least 3 matches, got " << match_count << '\n';
		return false;
	}
	return true;
}

bool TestPRF(DuckDB &db) {
	Connection con(db);
	auto &context = *con.context;

	std::vector<uint64_t> build_keys = {0, 1, 10, 11, 20, 21};
	std::vector<uint64_t> probes = {0, 2, 10, 12, 20, 22};

	auto filter = PrefixRangeFilter::CreatePrefixRangeFilter(LogicalType::UBIGINT);
	PrefixRangeFilter::Sizing sizing;
	if (!PrefixRangeFilter::TryComputeSizing(Value::UBIGINT(build_keys.front()), Value::UBIGINT(build_keys.back()),
	                                         build_keys.size(), sizing)) {
		std::cerr << "PRF sizing failed\n";
		return false;
	}
	filter->Initialize(context, build_keys.size(), Value::UBIGINT(build_keys.front()), Value::UBIGINT(build_keys.back()),
	                   sizing);

	Vector build_vec(LogicalType::UBIGINT, build_keys.size());
	Vector probe_vec(LogicalType::UBIGINT, probes.size());
	FillVector(build_vec, build_keys);
	FillVector(probe_vec, probes);

	auto state = filter->InitializeBuildState(context);
	filter->InsertKeys(build_vec, build_keys.size(), *state);
	filter->MergeBuildState(*state);
	filter->Compress(context, 0.001);
	const auto analysis = filter->Analyze();

	SelectionVector result_sel(probes.size());
	const auto match_count = filter->LookupKeys(probe_vec, result_sel, probes.size());
	if (match_count != 3) {
		std::cerr << "PRF sanity check failed: expected 3 exact matches, got " << match_count << '\n';
		return false;
	}
	if (analysis.active_buckets == 0) {
		std::cerr << "PRF sanity check failed: active buckets should be > 0\n";
		return false;
	}
	return true;
}

bool TestPRFDirectRanges(DuckDB &db) {
	Connection con(db);
	auto &context = *con.context;

	std::vector<uint64_t> build_keys = {0, 1, 2, 100, 101, 102};
	std::vector<uint64_t> probes = {0, 2, 3, 99, 100, 102, 103};

	auto filter = PrefixRangeFilter::CreatePrefixRangeFilter(LogicalType::UBIGINT);
	PrefixRangeFilter::Sizing sizing;
	if (!PrefixRangeFilter::TryComputeFixedSizeSizing(Value::UBIGINT(build_keys.front()),
	                                                  Value::UBIGINT(build_keys.back()), 103, sizing)) {
		std::cerr << "PRF direct-range fixed-size sizing failed\n";
		return false;
	}
	filter->Initialize(context, build_keys.size(), Value::UBIGINT(build_keys.front()), Value::UBIGINT(build_keys.back()),
	                   sizing);

	Vector build_vec(LogicalType::UBIGINT, build_keys.size());
	Vector probe_vec(LogicalType::UBIGINT, probes.size());
	FillVector(build_vec, build_keys);
	FillVector(probe_vec, probes);

	auto state = filter->InitializeBuildState(context);
	filter->InsertKeys(build_vec, build_keys.size(), *state);
	filter->MergeBuildState(*state);
	filter->Compress(context, 0.5);

	const auto info = filter->GetCompressionInfo();
	if (info.mode != CompressionMode::DIRECT_RANGES || info.range_count != 2) {
		std::cerr << "PRF direct-range compression failed: expected 2 direct ranges\n";
		return false;
	}

	SelectionVector result_sel(probes.size());
	const auto match_count = filter->LookupKeys(probe_vec, result_sel, probes.size());
	if (match_count != 4) {
		std::cerr << "PRF direct-range sanity check failed: expected 4 matches, got " << match_count << '\n';
		return false;
	}
	return true;
}

bool TestPRFRangeNoOverlap(DuckDB &db) {
	Connection con(db);
	auto &context = *con.context;
	std::vector<uint64_t> build_keys = {0, 1, 2, 97, 98, 99};
	auto filter = PrefixRangeFilter::CreatePrefixRangeFilter(LogicalType::UBIGINT);
	PrefixRangeFilter::Sizing sizing;
	if (!PrefixRangeFilter::TryComputeFixedSizeSizing(Value::UBIGINT(0), Value::UBIGINT(99), 100, sizing)) {
		return false;
	}
	filter->Initialize(context, build_keys.size(), Value::UBIGINT(0), Value::UBIGINT(99), sizing);
	Vector build_vec(LogicalType::UBIGINT, build_keys.size());
	FillVector(build_vec, build_keys);
	auto state = filter->InitializeBuildState(context);
	filter->InsertKeys(build_vec, build_keys.size(), *state);
	filter->MergeBuildState(*state);
	filter->Compress(context, 0.01);
	if (filter->LookupRange(Value::UBIGINT(40), Value::UBIGINT(59)) != FilterPropagateResult::FILTER_ALWAYS_FALSE) {
		std::cerr << "PRF range sanity check failed: empty middle range was not pruned\n";
		return false;
	}
	return true;
}

bool TestClusteredKeySpan() {
	const auto keys_two = GenerateClusteredKeysForTest(100, 10, 2);
	const auto keys_three = GenerateClusteredKeysForTest(100, 10, 3);
	const auto keys_one = GenerateClusteredKeysForTest(100, 10, 1);
	if (keys_two.back() != 99 || keys_three.back() != 99 || keys_one.back() != 9) {
		std::cerr << "Clustered key generator sanity check failed\n";
		return false;
	}
	return true;
}

#if defined(DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE)
bool TestGrafite() {
	std::vector<uint64_t> build_keys = {0, 1, 10, 11, 20, 21};
	GrafiteBenchmarkFilter filter;
	filter.Build(build_keys, 12.0);
	if (!filter.Probe(0) || !filter.Probe(10) || !filter.Probe(21)) {
		std::cerr << "Grafite sanity check failed: missing true positive\n";
		return false;
	}
	if (filter.SizeBytes() == 0) {
		std::cerr << "Grafite sanity check failed: size should be > 0\n";
		return false;
	}
	if (!filter.RangeProbe(0, 2)) {
		std::cerr << "Grafite range sanity check failed: overlapping range was rejected\n";
		return false;
	}
	return true;
}
#endif

#if defined(DUCKDB_FILTER_BENCHMARK_HAS_DIVA)
bool TestDiva() {
	std::vector<uint64_t> build_keys = {0, 1, 2, 97, 98, 99};
	DivaBenchmarkFilter filter;
	filter.Build(build_keys, 16.0);
	if (!filter.RangeProbe(0, 2)) {
		std::cerr << "Diva range sanity check failed: overlapping range was rejected\n";
		return false;
	}
	return true;
}
#endif

} // namespace

int main() {
	DuckDB db(nullptr);
	if (!TestBloom(db) || !TestPRF(db) || !TestPRFDirectRanges(db) || !TestPRFRangeNoOverlap(db) || !TestClusteredKeySpan()) {
		return 1;
	}
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE)
	if (!TestGrafite()) {
		return 1;
	}
#endif
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_DIVA)
	if (!TestDiva()) {
		return 1;
	}
#endif
	std::cout << "filter_benchmark_tests passed\n";
	return 0;
}
