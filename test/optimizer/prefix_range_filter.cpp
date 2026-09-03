#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/planner/filter/prefix_range_filter.hpp"

using namespace duckdb;

namespace {

unique_ptr<PrefixRangeFilter> BuildFinalizedInt32PrefixRangeFilter(ClientContext &context, const vector<int32_t> &keys,
                                                                   int32_t min, int32_t max, idx_t shift,
                                                                   double max_false_positive_rate = 1.0) {
	auto filter = PrefixRangeFilter::CreatePrefixRangeFilter(LogicalType::INTEGER);
	PrefixRangeFilter::Sizing sizing;
	REQUIRE(PrefixRangeFilter::TryComputeSpan(Value::INTEGER(min), Value::INTEGER(max), sizing.span));
	sizing.shift = shift;
	filter->Initialize(context, MaxValue<idx_t>(keys.size(), 1), Value::INTEGER(min), Value::INTEGER(max), sizing);

	auto state = filter->InitializeBuildState(context);
	Vector key_vector(LogicalType::INTEGER, keys.size());
	auto key_data = FlatVector::GetDataMutable<int32_t>(key_vector);
	for (idx_t i = 0; i < keys.size(); i++) {
		key_data[i] = keys[i];
	}
	FlatVector::SetSize(key_vector, count_t(keys.size()));
	filter->InsertKeys(key_vector, *state);
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

bool ContainsAllKeys(const PrefixRangeFilter &filter, const vector<int32_t> &keys) {
	Vector key_vector(LogicalType::INTEGER, keys.size());
	auto key_data = FlatVector::GetDataMutable<int32_t>(key_vector);
	for (idx_t i = 0; i < keys.size(); i++) {
		key_data[i] = keys[i];
	}
	FlatVector::SetSize(key_vector, count_t(keys.size()));
	SelectionVector result_sel(keys.size());
	return filter.LookupKeys(key_vector, result_sel, keys.size()) == keys.size();
}

} // namespace

TEST_CASE("Prefix range filter finalizes one exact run as a direct range", "[optimizer][prefix_range_filter]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<int32_t> keys;
	for (int32_t key = 100; key < 200; key++) {
		keys.push_back(key);
	}
	auto filter = BuildFinalizedInt32PrefixRangeFilter(*con.context, keys, 100, 199, 0);
	filter->Compress(*con.context, 1.0);

	const auto info = filter->GetCompressionInfo();
	REQUIRE(info.mode == PrefixRangeFilter::CompressionMode::DIRECT_RANGES);
	REQUIRE(info.range_count == 1);
	REQUIRE(info.run_count == 1);
	REQUIRE(info.active_buckets == keys.size());
	REQUIRE(info.bitmap_allocation_bytes == 0);
	REQUIRE(info.false_positive_rate == 0);
	REQUIRE(ContainsKey(*filter, 100));
	REQUIRE(ContainsKey(*filter, 150));
	REQUIRE(ContainsKey(*filter, 199));
	REQUIRE_FALSE(ContainsKey(*filter, 99));
	REQUIRE_FALSE(ContainsKey(*filter, 200));
}

TEST_CASE("Prefix range filter preserves four exact ranges", "[optimizer][prefix_range_filter]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<int32_t> keys;
	for (int32_t base : {0, 100, 300, 700}) {
		for (int32_t key = base; key < base + 10; key++) {
			keys.push_back(key);
		}
	}
	auto filter = BuildFinalizedInt32PrefixRangeFilter(*con.context, keys, 0, 709, 0);

	const auto info = filter->GetCompressionInfo();
	REQUIRE(info.mode == PrefixRangeFilter::CompressionMode::DIRECT_RANGES);
	REQUIRE(info.range_count == 4);
	REQUIRE(info.run_count == 4);
	REQUIRE(info.bitmap_allocation_bytes == 0);
	for (auto key : keys) {
		REQUIRE(ContainsKey(*filter, key));
	}
	REQUIRE_FALSE(ContainsKey(*filter, 50));
	REQUIRE_FALSE(ContainsKey(*filter, 250));
	REQUIRE_FALSE(ContainsKey(*filter, 500));
}

TEST_CASE("Prefix range filter retains bitmaps with more than four runs", "[optimizer][prefix_range_filter]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto filter = BuildFinalizedInt32PrefixRangeFilter(*con.context, {0, 10, 20, 30, 40}, 0, 40, 0);
	const auto info = filter->GetCompressionInfo();
	REQUIRE(info.mode == PrefixRangeFilter::CompressionMode::BITMAP);
	REQUIRE(info.range_count == 0);
	REQUIRE(info.run_count == 5);
	REQUIRE(info.bitmap_allocation_bytes > 0);
	REQUIRE(ContainsKey(*filter, 20));
	REQUIRE_FALSE(ContainsKey(*filter, 21));
}

TEST_CASE("Prefix range filter retains an empty bitmap", "[optimizer][prefix_range_filter]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto filter = BuildFinalizedInt32PrefixRangeFilter(*con.context, {}, 0, 100, 0);
	const auto info = filter->GetCompressionInfo();
	REQUIRE(info.mode == PrefixRangeFilter::CompressionMode::BITMAP);
	REQUIRE(info.active_buckets == 0);
	REQUIRE(info.run_count == 0);
	REQUIRE(info.range_count == 0);
	REQUIRE(info.bitmap_allocation_bytes > 0);
	REQUIRE_FALSE(ContainsKey(*filter, 0));
	REQUIRE_FALSE(ContainsKey(*filter, 50));
}

TEST_CASE("Prefix range filter direct ranges retain bucket false positives", "[optimizer][prefix_range_filter]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto filter = BuildFinalizedInt32PrefixRangeFilter(*con.context, {0, 8}, 0, 15, 2);
	const auto info = filter->GetCompressionInfo();
	REQUIRE(info.mode == PrefixRangeFilter::CompressionMode::DIRECT_RANGES);
	REQUIRE(info.range_count == 2);
	REQUIRE(info.active_buckets == 2);
	REQUIRE(ContainsKey(*filter, 0));
	REQUIRE(ContainsKey(*filter, 2));
	REQUIRE_FALSE(ContainsKey(*filter, 4));
	REQUIRE(ContainsKey(*filter, 10));
	REQUIRE_FALSE(ContainsKey(*filter, 12));
	REQUIRE(filter->LookupRange(Value::INTEGER(4), Value::INTEGER(7)) == FilterPropagateResult::FILTER_ALWAYS_FALSE);
	REQUIRE(filter->LookupRange(Value::INTEGER(2), Value::INTEGER(4)) == FilterPropagateResult::NO_PRUNING_POSSIBLE);
}

TEST_CASE("Prefix range filter direct ranges preserve signed ordering", "[optimizer][prefix_range_filter]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto filter = BuildFinalizedInt32PrefixRangeFilter(*con.context, {-3, -2, 0, 7}, -3, 7, 0);
	const auto info = filter->GetCompressionInfo();
	REQUIRE(info.mode == PrefixRangeFilter::CompressionMode::DIRECT_RANGES);
	REQUIRE(info.range_count == 3);
	REQUIRE(ContainsKey(*filter, -3));
	REQUIRE(ContainsKey(*filter, 0));
	REQUIRE(ContainsKey(*filter, 7));
	REQUIRE_FALSE(ContainsKey(*filter, -1));
	REQUIRE_FALSE(ContainsKey(*filter, 6));
	REQUIRE(filter->LookupRange(Value::INTEGER(-1), Value::INTEGER(-1)) == FilterPropagateResult::FILTER_ALWAYS_FALSE);
	REQUIRE(filter->LookupRange(Value::INTEGER(2), Value::INTEGER(6)) == FilterPropagateResult::FILTER_ALWAYS_FALSE);
	REQUIRE(filter->LookupRange(Value::INTEGER(-2), Value::INTEGER(0)) == FilterPropagateResult::NO_PRUNING_POSSIBLE);
}

TEST_CASE("Prefix range filter direct lookup preserves selection positions", "[optimizer][prefix_range_filter]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto filter = BuildFinalizedInt32PrefixRangeFilter(*con.context, {0, 100}, 0, 100, 0);
	Vector key_vector(LogicalType::INTEGER, 4);
	auto key_data = FlatVector::GetDataMutable<int32_t>(key_vector);
	key_data[0] = 0;
	key_data[1] = 50;
	key_data[2] = 100;
	key_data[3] = 101;
	FlatVector::SetSize(key_vector, 4);

	SelectionVector input_sel(3);
	input_sel.set_index(0, 1);
	input_sel.set_index(1, 2);
	input_sel.set_index(2, 3);
	SelectionVector result_sel(3);
	REQUIRE(filter->LookupKeys(key_vector, input_sel, result_sel, 3) == 1);
	REQUIRE(result_sel.get_index(0) == 1);
}

TEST_CASE("Prefix range filter stops at the exact cache target", "[optimizer][prefix_range_filter]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<int32_t> keys;
	for (int32_t key = 0; key < 131072; key += 32) {
		keys.push_back(key);
	}
	auto filter = BuildFinalizedInt32PrefixRangeFilter(*con.context, keys, 0, 131071, 0, 1.0);
	const auto info = filter->GetCompressionInfo();
	REQUIRE(info.mode == PrefixRangeFilter::CompressionMode::BITMAP);
	REQUIRE(info.shift == 0);
	REQUIRE(info.logical_bucket_count == 131072);
	REQUIRE(info.bitmap_allocation_bytes == 64 + 2048 * sizeof(uint64_t));
	REQUIRE(info.run_count == keys.size());
	REQUIRE(info.false_positive_rate == 0);
	REQUIRE(ContainsAllKeys(*filter, keys));
}

TEST_CASE("Prefix range filter rejects compression above its false-positive budget",
          "[optimizer][prefix_range_filter]") {
	DuckDB db(nullptr);
	Connection con(db);

	static constexpr double MAX_FALSE_POSITIVE_RATE = 0.5;
	auto filter =
	    BuildFinalizedInt32PrefixRangeFilter(*con.context, {0, 16, 32, 48}, 0, 64, 4, MAX_FALSE_POSITIVE_RATE);
	const auto info = filter->GetCompressionInfo();
	const auto analysis = filter->Analyze();
	REQUIRE(info.mode == PrefixRangeFilter::CompressionMode::BITMAP);
	REQUIRE(info.shift == 4);
	REQUIRE(info.active_buckets == 4);
	REQUIRE(info.run_count == 1);
	REQUIRE(info.false_positive_rate > MAX_FALSE_POSITIVE_RATE);
	REQUIRE(analysis.active_buckets == info.active_buckets);
	REQUIRE(analysis.false_positive_rate == Approx(info.false_positive_rate));
}

TEST_CASE("Prefix range filter stops lossless compression at the cache target", "[optimizer][prefix_range_filter]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<int32_t> keys;
	for (int32_t base : {0, 4, 8, 12, 16}) {
		keys.push_back(base);
		keys.push_back(base + 1);
	}
	static constexpr double MAX_FALSE_POSITIVE_RATE = 0.1;
	auto filter = BuildFinalizedInt32PrefixRangeFilter(*con.context, keys, 0, 17, 0, MAX_FALSE_POSITIVE_RATE);
	const auto info = filter->GetCompressionInfo();
	const auto analysis = filter->Analyze();
	REQUIRE(info.mode == PrefixRangeFilter::CompressionMode::BITMAP);
	REQUIRE(info.shift == 0);
	REQUIRE(info.active_buckets == keys.size());
	REQUIRE(info.run_count == 5);
	REQUIRE(info.false_positive_rate == 0);
	REQUIRE(analysis.active_buckets == info.active_buckets);
	REQUIRE(analysis.false_positive_rate == info.false_positive_rate);
	for (auto key : keys) {
		REQUIRE(ContainsKey(*filter, key));
	}
	REQUIRE_FALSE(ContainsKey(*filter, 2));
	REQUIRE_FALSE(ContainsKey(*filter, 6));
}

TEST_CASE("Prefix range filter discovers direct ranges at a coarser level", "[optimizer][prefix_range_filter]") {
	DuckDB db(nullptr);
	Connection con(db);

	const vector<int32_t> keys {0, 1, 3, 4, 6, 7, 9, 10, 12, 13};
	auto filter = BuildFinalizedInt32PrefixRangeFilter(*con.context, keys, 0, 200000, 0, 0.001);
	const auto info = filter->GetCompressionInfo();
	REQUIRE(info.mode == PrefixRangeFilter::CompressionMode::DIRECT_RANGES);
	REQUIRE(info.shift == 1);
	REQUIRE(info.range_count == 1);
	REQUIRE(info.run_count == 1);
	REQUIRE(info.bitmap_allocation_bytes == 0);
	REQUIRE(info.false_positive_rate == Approx(4.0 / 199991.0));
	for (auto key : keys) {
		REQUIRE(ContainsKey(*filter, key));
	}
}

TEST_CASE("Prefix range filter retains the last candidate within its false-positive budget",
          "[optimizer][prefix_range_filter]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<int32_t> keys;
	for (int32_t key = 0; key < 1048576; key += 32) {
		keys.push_back(key);
	}
	auto filter = BuildFinalizedInt32PrefixRangeFilter(*con.context, keys, 0, 1048575, 0, 0.05);
	const auto info = filter->GetCompressionInfo();
	REQUIRE(info.mode == PrefixRangeFilter::CompressionMode::BITMAP);
	REQUIRE(info.shift == 1);
	REQUIRE(info.logical_bucket_count == 524288);
	REQUIRE(info.bitmap_allocation_bytes == 64 + 8192 * sizeof(uint64_t));
	REQUIRE(info.false_positive_rate < 0.05);
	REQUIRE(ContainsAllKeys(*filter, keys));
}

TEST_CASE("Prefix range filter reuses storage and right-sizes the retained bitmap",
          "[optimizer][prefix_range_filter]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<int32_t> keys;
	for (int32_t key = 0; key < 1048576; key += 32) {
		keys.push_back(key);
	}
	auto filter = BuildFinalizedInt32PrefixRangeFilter(*con.context, keys, 0, 1048575, 0, 1.0);
	const auto info = filter->GetCompressionInfo();
	const auto first_analysis = filter->Analyze();
	const auto second_analysis = filter->Compress(*con.context, 1.0);
	const auto repeated_info = filter->GetCompressionInfo();

	REQUIRE(info.mode == PrefixRangeFilter::CompressionMode::BITMAP);
	REQUIRE(info.shift == 3);
	REQUIRE(info.logical_bucket_count == 131072);
	REQUIRE(info.bitmap_allocation_bytes == 64 + 2048 * sizeof(uint64_t));
	REQUIRE(first_analysis.active_buckets == info.active_buckets);
	REQUIRE(first_analysis.false_positive_rate == Approx(info.false_positive_rate));
	REQUIRE(second_analysis.active_buckets == first_analysis.active_buckets);
	REQUIRE(second_analysis.false_positive_rate == Approx(first_analysis.false_positive_rate));
	REQUIRE(repeated_info.shift == info.shift);
	REQUIRE(repeated_info.bitmap_allocation_bytes == info.bitmap_allocation_bytes);
	REQUIRE(ContainsAllKeys(*filter, keys));
}
