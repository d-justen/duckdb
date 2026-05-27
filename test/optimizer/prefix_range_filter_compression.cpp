#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"

using namespace duckdb;

namespace {

unique_ptr<PrefixRangeFilter> BuildInt32PrefixRangeFilter(ClientContext &context, const vector<int32_t> &keys,
                                                          int32_t min, int32_t max, idx_t shift,
                                                          double max_false_positive_rate) {
	auto filter = PrefixRangeFilter::CreatePrefixRangeFilter(LogicalType::INTEGER);
	PrefixRangeFilter::Sizing sizing;
	REQUIRE(PrefixRangeFilter::TryComputeSpan(Value::INTEGER(min), Value::INTEGER(max), sizing.span));
	sizing.shift = shift;
	filter->Initialize(context, keys.size(), Value::INTEGER(min), Value::INTEGER(max), sizing);

	auto state = filter->InitializeBuildState(context);
	Vector key_vector(LogicalType::INTEGER, keys.size());
	auto key_data = FlatVector::GetDataMutable<int32_t>(key_vector);
	for (idx_t i = 0; i < keys.size(); i++) {
		key_data[i] = keys[i];
	}
	FlatVector::SetSize(key_vector, count_t(keys.size()));
	filter->InsertKeys(key_vector, keys.size(), *state);
	filter->MergeBuildState(*state);
	filter->Compress(context, max_false_positive_rate);
	return filter;
}

bool ContainsKey(const PrefixRangeFilter &filter, int32_t key) {
	Vector key_vector(LogicalType::INTEGER, 1);
	FlatVector::GetDataMutable<int32_t>(key_vector)[0] = key;
	FlatVector::SetSize(key_vector, 1);
	SelectionVector result_sel(1);
	return filter.LookupKeys(key_vector, result_sel, 1) == 1;
}

} // namespace

TEST_CASE("Prefix range filter direct compression uses one range for contiguous values", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<int32_t> keys;
	for (int32_t key = 100; key < 200; key++) {
		keys.push_back(key);
	}

	auto filter = BuildInt32PrefixRangeFilter(*con.context, keys, 100, 199, 0, 0);
	auto info = filter->GetCompressionInfo();
	REQUIRE(info.mode == PrefixRangeFilter::CompressionMode::DIRECT_RANGES);
	REQUIRE(info.range_count == 1);
	REQUIRE(info.false_positive_rate == 0);
	for (auto key : keys) {
		REQUIRE(ContainsKey(*filter, key));
	}
}

TEST_CASE("Prefix range filter direct compression preserves four sparse value ranges", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<int32_t> keys;
	for (int32_t base : {0, 100, 300, 700}) {
		for (int32_t key = base; key < base + 10; key++) {
			keys.push_back(key);
		}
	}

	auto filter = BuildInt32PrefixRangeFilter(*con.context, keys, 0, 709, 0, 0);
	auto info = filter->GetCompressionInfo();
	REQUIRE(info.mode == PrefixRangeFilter::CompressionMode::DIRECT_RANGES);
	REQUIRE(info.range_count == 4);
	REQUIRE(info.false_positive_rate == 0);
	for (auto key : keys) {
		REQUIRE(ContainsKey(*filter, key));
	}
	REQUIRE(!ContainsKey(*filter, 50));
	REQUIRE(!ContainsKey(*filter, 250));
	REQUIRE(!ContainsKey(*filter, 500));
}

TEST_CASE("Prefix range filter falls back to dyadic compression when four ranges exceed FPR", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<int32_t> keys;
	for (int32_t key = 0; key <= 60; key += 4) {
		keys.push_back(key);
	}

	auto filter = BuildInt32PrefixRangeFilter(*con.context, keys, 0, 64, 0, 0.34);
	auto info = filter->GetCompressionInfo();
	REQUIRE(info.mode == PrefixRangeFilter::CompressionMode::BITMAP);
	REQUIRE(info.shift == 1);
	REQUIRE(info.false_positive_rate <= 0.34);
	for (auto key : keys) {
		REQUIRE(ContainsKey(*filter, key));
	}
}

TEST_CASE("Prefix range filter FPR analysis is conservative for duplicate build keys", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<int32_t> keys;
	for (idx_t repeat = 0; repeat < 100; repeat++) {
		for (int32_t key : {0, 16, 32, 48}) {
			keys.push_back(key);
		}
	}

	auto filter = BuildInt32PrefixRangeFilter(*con.context, keys, 0, 64, 4, 1.0);
	auto analysis = filter->Analyze(keys.size());
	REQUIRE(analysis.active_buckets == 4);
	REQUIRE(analysis.false_positive_rate > 0.9);
}
