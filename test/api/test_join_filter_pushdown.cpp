#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

using namespace duckdb;

TEST_CASE("Bloom filter row group pruning can be disabled independently", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;

	auto stats = NumericStats::CreateEmpty(LogicalType::INTEGER);
	stats.SetHasNoNullFast();
	NumericStats::SetMin<int32_t>(stats, 1000);
	NumericStats::SetMax<int32_t>(stats, 1005);

	BloomFilter uninitialized_filter;
	BloomFilterFunctionData uninitialized_prune_enabled(&uninitialized_filter, true, "k", LogicalType::INTEGER, 0.0f,
	                                                    0, true);
	FunctionStatisticsPruneInput uninitialized_enabled_input(&uninitialized_prune_enabled, stats);
	REQUIRE(BloomFilterScalarFun::FilterPrune(uninitialized_enabled_input) ==
	        FilterPropagateResult::FILTER_ALWAYS_TRUE);

	BloomFilterFunctionData uninitialized_prune_disabled(&uninitialized_filter, true, "k", LogicalType::INTEGER, 0.0f,
	                                                     0, false);
	FunctionStatisticsPruneInput uninitialized_disabled_input(&uninitialized_prune_disabled, stats);
	REQUIRE(BloomFilterScalarFun::FilterPrune(uninitialized_disabled_input) ==
	        FilterPropagateResult::FILTER_ALWAYS_TRUE);

	BloomFilter filter;
	filter.Initialize(context, 1);
	Vector hashes(Value::HASH(Hash(int64_t(42))), count_t(1));
	hashes.Flatten();
	filter.InsertHashes(hashes, 1);

	BloomFilterFunctionData prune_enabled(&filter, true, "k", LogicalType::INTEGER, 0.0f, 0, true);
	FunctionStatisticsPruneInput enabled_input(&prune_enabled, stats);
	REQUIRE(BloomFilterScalarFun::FilterPrune(enabled_input) == FilterPropagateResult::FILTER_ALWAYS_FALSE);

	BloomFilterFunctionData prune_disabled(&filter, true, "k", LogicalType::INTEGER, 0.0f, 0, false);
	FunctionStatisticsPruneInput disabled_input(&prune_disabled, stats);
	REQUIRE(BloomFilterScalarFun::FilterPrune(disabled_input) == FilterPropagateResult::NO_PRUNING_POSSIBLE);
}
