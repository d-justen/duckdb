#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

using namespace duckdb;

TEST_CASE("Bloom filter row group pruning can be disabled independently", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;

	BloomFilter filter;
	filter.Initialize(context, 1);
	Vector hashes(Value::HASH(Hash(int64_t(42))), count_t(1));
	hashes.Flatten();
	filter.InsertHashes(hashes);

	auto stats = NumericStats::CreateEmpty(LogicalType::INTEGER);
	stats.SetHasNoNullFast();
	NumericStats::SetMin<int32_t>(stats, 1000);
	NumericStats::SetMax<int32_t>(stats, 1005);

	vector<unique_ptr<Expression>> children;
	children.push_back(make_uniq<BoundReferenceExpression>(LogicalType::INTEGER, 0));
	BoundFunctionExpression expression(BoundScalarFunction(BloomFilterScalarFun::GetFunction(LogicalType::INTEGER)),
	                                   std::move(children), nullptr);
	vector<optional_ptr<const BaseStatistics>> child_stats {&stats};

	BloomFilterFunctionData enabled(&filter, true, "k", LogicalType::INTEGER, 0.0f, 0, true);
	FunctionStatisticsPruneInput enabled_input(expression, &enabled, child_stats);
	REQUIRE(BloomFilterScalarFun::FilterPrune(enabled_input) == FilterPropagateResult::FILTER_ALWAYS_FALSE);

	BloomFilterFunctionData disabled(&filter, true, "k", LogicalType::INTEGER, 0.0f, 0, false);
	FunctionStatisticsPruneInput disabled_input(expression, &disabled, child_stats);
	REQUIRE(BloomFilterScalarFun::FilterPrune(disabled_input) == FilterPropagateResult::NO_PRUNING_POSSIBLE);
}
