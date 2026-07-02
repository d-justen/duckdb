#include "duckdb.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"

#include <iostream>
#include <vector>

using namespace duckdb;

namespace {

void FillVector(Vector &vector, const std::vector<uint64_t> &values) {
	auto data = FlatVector::GetDataMutable<uint64_t>(vector);
	for (idx_t i = 0; i < values.size(); i++) {
		data[i] = values[i];
	}
	FlatVector::SetSize(vector, values.size());
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

} // namespace

int main() {
	DuckDB db(nullptr);
	if (!TestBloom(db) || !TestPRF(db)) {
		return 1;
	}
	std::cout << "filter_benchmark_tests passed\n";
	return 0;
}
