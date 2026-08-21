#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

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

unique_ptr<PrefixRangeFilter> BuildStringPrefixRangeFilter(ClientContext &context, const vector<string> &keys,
                                                           const string &min, const string &max) {
	auto filter = PrefixRangeFilter::CreatePrefixRangeFilter(LogicalType::VARCHAR);
	PrefixRangeFilter::Sizing sizing;
	REQUIRE(PrefixRangeFilter::TryComputeFixedSizeSizing(Value(min), Value(max), 1 << 16, sizing));
	filter->Initialize(context, keys.size(), Value(min), Value(max), sizing);

	auto state = filter->InitializeBuildState(context);
	Vector key_vector(LogicalType::VARCHAR, keys.size());
	auto key_data = FlatVector::GetDataMutable<string_t>(key_vector);
	for (idx_t i = 0; i < keys.size(); i++) {
		key_data[i] = StringVector::AddString(key_vector, keys[i]);
	}
	FlatVector::SetSize(key_vector, count_t(keys.size()));
	filter->InsertKeys(key_vector, keys.size(), *state);
	filter->MergeBuildState(*state);
	return filter;
}

BaseStatistics StringStatistics(const string &min, StringStatsType min_type, const string &max,
                                StringStatsType max_type) {
	auto stats = StringStats::CreateEmpty(LogicalType::VARCHAR);
	StringStats::SetMin(stats, string_t(min.data(), UnsafeNumericCast<uint32_t>(min.size())), min_type);
	StringStats::SetMax(stats, string_t(max.data(), UnsafeNumericCast<uint32_t>(max.size())), max_type);
	return stats;
}

BaseStatistics Int32Statistics(int32_t min, int32_t max) {
	auto stats = NumericStats::CreateEmpty(LogicalType::INTEGER);
	NumericStats::SetMin(stats, min);
	NumericStats::SetMax(stats, max);
	return stats;
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
	REQUIRE(info.mode == CompressionMode::DIRECT_RANGES);
	REQUIRE(info.range_count == 1);
	REQUIRE(info.run_count == 1);
	REQUIRE(info.bitmap_allocation_bytes == 0);
	REQUIRE(info.false_positive_rate == 0);
	for (auto key : keys) {
		REQUIRE(ContainsKey(*filter, key));
	}
}

TEST_CASE("Prefix range filter compresses exact threshold bitmap to one direct range", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	static constexpr int32_t KEY_COUNT = 1 << 23;
	vector<int32_t> keys;
	keys.reserve(KEY_COUNT);
	for (int32_t key = 0; key < KEY_COUNT; key++) {
		keys.push_back(key);
	}

	auto filter = BuildInt32PrefixRangeFilter(*con.context, keys, 0, KEY_COUNT - 1, 0, 0.001);
	auto info = filter->GetCompressionInfo();
	REQUIRE(info.mode == CompressionMode::DIRECT_RANGES);
	REQUIRE(info.range_count == 1);
	REQUIRE(info.bitmap_allocation_bytes == 0);
	REQUIRE(info.false_positive_rate == 0);
	REQUIRE(ContainsKey(*filter, 0));
	REQUIRE(ContainsKey(*filter, KEY_COUNT - 1));
	REQUIRE(!ContainsKey(*filter, KEY_COUNT));
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
	REQUIRE(info.mode == CompressionMode::DIRECT_RANGES);
	REQUIRE(info.range_count == 4);
	REQUIRE(info.run_count == 4);
	REQUIRE(info.bitmap_allocation_bytes == 0);
	REQUIRE(info.false_positive_rate == 0);
	for (auto key : keys) {
		REQUIRE(ContainsKey(*filter, key));
	}
	REQUIRE(!ContainsKey(*filter, 50));
	REQUIRE(!ContainsKey(*filter, 250));
	REQUIRE(!ContainsKey(*filter, 500));
}

TEST_CASE("Prefix range filter keeps a small bitmap when dyadic compression would add false positives", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<int32_t> keys;
	for (int32_t key = 0; key <= 60; key += 4) {
		keys.push_back(key);
	}

	auto filter = BuildInt32PrefixRangeFilter(*con.context, keys, 0, 64, 0, 0.34);
	auto info = filter->GetCompressionInfo();
	REQUIRE(info.mode == CompressionMode::BITMAP);
	REQUIRE(info.shift == 0);
	REQUIRE(info.run_count == keys.size());
	REQUIRE(info.false_positive_rate == 0);
	REQUIRE(info.false_positive_rate <= 0.34);
	for (auto key : keys) {
		REQUIRE(ContainsKey(*filter, key));
	}
}

TEST_CASE("Prefix range filter rejects an initially oversized FPR before compression", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	static constexpr double MAX_FALSE_POSITIVE_RATE = 0.5;
	auto filter = BuildInt32PrefixRangeFilter(*con.context, {0, 16, 32, 48}, 0, 64, 4, MAX_FALSE_POSITIVE_RATE);
	auto info = filter->GetCompressionInfo();
	auto analysis = filter->Analyze();

	REQUIRE(info.mode == CompressionMode::BITMAP);
	REQUIRE(info.shift == 4);
	REQUIRE(info.active_buckets == 4);
	REQUIRE(info.run_count == 1);
	REQUIRE(info.bitmap_allocation_bytes > 0);
	REQUIRE(info.false_positive_rate > MAX_FALSE_POSITIVE_RATE);
	REQUIRE(analysis.active_buckets == info.active_buckets);
	REQUIRE(analysis.false_positive_rate == Approx(info.false_positive_rate));
}

TEST_CASE("Prefix range filter dyadic analysis preserves original positive lower bound", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<int32_t> keys;
	for (int32_t base : {0, 4, 8, 12, 16}) {
		keys.push_back(base);
		keys.push_back(base + 1);
	}

	static constexpr double MAX_FALSE_POSITIVE_RATE = 0.1;
	auto filter = BuildInt32PrefixRangeFilter(*con.context, keys, 0, 17, 0, MAX_FALSE_POSITIVE_RATE);
	auto info = filter->GetCompressionInfo();
	auto analysis = filter->Analyze();

	REQUIRE(info.mode == CompressionMode::BITMAP);
	REQUIRE(info.shift == 1);
	REQUIRE(info.active_buckets == 5);
	REQUIRE(info.false_positive_rate == Approx(0.0));
	REQUIRE(info.false_positive_rate <= MAX_FALSE_POSITIVE_RATE);
	REQUIRE(analysis.active_buckets == info.active_buckets);
	REQUIRE(analysis.false_positive_rate == Approx(info.false_positive_rate));
	REQUIRE(analysis.false_positive_rate <= MAX_FALSE_POSITIVE_RATE);
	for (auto key : keys) {
		REQUIRE(ContainsKey(*filter, key));
	}
	REQUIRE(!ContainsKey(*filter, 2));
	REQUIRE(!ContainsKey(*filter, 6));
}

TEST_CASE("Prefix range filter fast-builds exact ranges discovered by dyadic compression", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto filter = BuildInt32PrefixRangeFilter(*con.context, {0, 1, 3, 4, 6, 7, 9, 10, 12, 13}, 0, 13, 0, 1.0);
	auto info = filter->GetCompressionInfo();

	REQUIRE(info.mode == CompressionMode::DIRECT_RANGES);
	REQUIRE(info.shift == 1);
	REQUIRE(info.range_count == 1);
	REQUIRE(info.run_count == 1);
	REQUIRE(info.bitmap_allocation_bytes == 0);
	REQUIRE(info.false_positive_rate == Approx(1.0));
	for (int32_t key : {0, 1, 3, 4, 6, 7, 9, 10, 12, 13}) {
		REQUIRE(ContainsKey(*filter, key));
	}
}

TEST_CASE("Prefix range filter right-sizes an accepted cache-sized bitmap", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<int32_t> keys;
	for (int32_t key = 0; key <= 262140; key += 6) {
		keys.push_back(key);
	}
	auto filter = BuildInt32PrefixRangeFilter(*con.context, keys, 0, 262140, 0, 1.0);
	auto info = filter->GetCompressionInfo();
	const auto first_analysis = filter->Analyze();
	const auto second_analysis = filter->Compress(*con.context, 1.0);
	const auto repeated_info = filter->GetCompressionInfo();

	REQUIRE(info.mode == CompressionMode::BITMAP);
	REQUIRE(info.shift == 1);
	REQUIRE(info.logical_bucket_count == 131071);
	REQUIRE(info.bitmap_allocation_bytes == 64 + 2048 * sizeof(uint64_t));
	REQUIRE(info.run_count == keys.size());
	REQUIRE(first_analysis.active_buckets == info.active_buckets);
	REQUIRE(first_analysis.false_positive_rate == Approx(info.false_positive_rate));
	REQUIRE(second_analysis.active_buckets == first_analysis.active_buckets);
	REQUIRE(second_analysis.false_positive_rate == Approx(first_analysis.false_positive_rate));
	REQUIRE(repeated_info.shift == info.shift);
	REQUIRE(repeated_info.bitmap_allocation_bytes == info.bitmap_allocation_bytes);
	REQUIRE(ContainsKey(*filter, keys.front()));
	REQUIRE(ContainsKey(*filter, keys.back()));
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
	auto analysis = filter->Analyze();
	REQUIRE(analysis.active_buckets == 4);
	REQUIRE(analysis.false_positive_rate > 0.9);
}

TEST_CASE("Prefix range filter direct range analysis stays consistent after compression", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto filter = BuildInt32PrefixRangeFilter(*con.context, {0, 1, 2, 3, 8, 9, 10, 11}, 0, 11, 0, 0);
	auto info = filter->GetCompressionInfo();
	auto analysis = filter->Analyze();

	REQUIRE(info.mode == CompressionMode::DIRECT_RANGES);
	REQUIRE(analysis.active_buckets == info.active_buckets);
	REQUIRE(analysis.false_positive_rate == Approx(info.false_positive_rate));
}

TEST_CASE("Prefix range filter bitmap statistics return always true for fully covered range", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto filter = BuildInt32PrefixRangeFilter(*con.context, {0, 1, 2, 3}, 0, 7, 1, -1);

	REQUIRE(filter->LookupStatistics(Int32Statistics(0, 1)) == FilterPropagateResult::FILTER_ALWAYS_TRUE);
	REQUIRE(filter->LookupStatistics(Int32Statistics(4, 5)) == FilterPropagateResult::FILTER_ALWAYS_FALSE);
	REQUIRE(filter->LookupStatistics(Int32Statistics(1, 4)) == FilterPropagateResult::NO_PRUNING_POSSIBLE);
	REQUIRE(filter->LookupStatistics(Int32Statistics(-10, 100)) == FilterPropagateResult::NO_PRUNING_POSSIBLE);
}

TEST_CASE("Prefix range filter indexes long homogeneous bitmap word runs", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<int32_t> keys;
	keys.reserve(140004);
	for (int32_t key = 1; key <= 140000; key++) {
		keys.push_back(key);
	}
	for (int32_t key : {300000, 500000, 700000, 900000}) {
		keys.push_back(key);
	}

	auto filter = BuildInt32PrefixRangeFilter(*con.context, keys, 0, 1100000, 0, 0);
	const auto info = filter->GetCompressionInfo();
	REQUIRE(info.mode == CompressionMode::BITMAP);
	REQUIRE(info.shift == 0);
	REQUIRE(info.range_index_count > 0);
	REQUIRE(info.range_index_bytes >= info.range_index_count * 2 * sizeof(idx_t));

	// The boundary words are included in these checks; only the complete interior words use the index.
	REQUIRE(filter->LookupStatistics(Int32Statistics(64, 139967)) == FilterPropagateResult::FILTER_ALWAYS_TRUE);
	REQUIRE(filter->LookupStatistics(Int32Statistics(140001, 299999)) == FilterPropagateResult::FILTER_ALWAYS_FALSE);
	REQUIRE(filter->LookupStatistics(Int32Statistics(140001, 499999)) == FilterPropagateResult::NO_PRUNING_POSSIBLE);

	// Short and single-word ranges continue to use exact boundary masks.
	REQUIRE(filter->LookupStatistics(Int32Statistics(0, 0)) == FilterPropagateResult::FILTER_ALWAYS_FALSE);
	REQUIRE(filter->LookupStatistics(Int32Statistics(1, 1)) == FilterPropagateResult::FILTER_ALWAYS_TRUE);
}

TEST_CASE("Prefix range filter direct ranges return always true for fully covered range", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto filter = BuildInt32PrefixRangeFilter(*con.context, {0, 1, 2, 3, 8, 9, 10, 11}, 0, 11, 0, 0);
	auto info = filter->GetCompressionInfo();
	REQUIRE(info.mode == CompressionMode::DIRECT_RANGES);
	REQUIRE(info.range_count == 2);

	REQUIRE(filter->LookupStatistics(Int32Statistics(0, 3)) == FilterPropagateResult::FILTER_ALWAYS_TRUE);
	REQUIRE(filter->LookupStatistics(Int32Statistics(4, 7)) == FilterPropagateResult::FILTER_ALWAYS_FALSE);
	REQUIRE(filter->LookupStatistics(Int32Statistics(2, 9)) == FilterPropagateResult::NO_PRUNING_POSSIBLE);
}

TEST_CASE("Prefix range filter optional selectivity threshold is lower than min max", "[optimizer]") {
	static constexpr idx_t DOMAIN_SIZE = 100000;
	static constexpr idx_t CONTIGUOUS_RANGE_SIZE = 83886;

	float prf_threshold;
	idx_t prf_vectors_to_check;
	GetThresholdAndVectorsToCheck(SelectivityOptionalFilterType::PRF, prf_threshold, prf_vectors_to_check);

	SelectivityOptionalFilterState::SelectivityStats prf_stats(prf_vectors_to_check, prf_threshold);
	for (idx_t i = 0; i < prf_vectors_to_check; i++) {
		prf_stats.Update(CONTIGUOUS_RANGE_SIZE, DOMAIN_SIZE);
	}
	REQUIRE(prf_stats.GetSelectivity() == Approx(0.83886));
	REQUIRE(!prf_stats.IsActive());

	float min_max_threshold;
	idx_t min_max_vectors_to_check;
	GetThresholdAndVectorsToCheck(SelectivityOptionalFilterType::MIN_MAX, min_max_threshold, min_max_vectors_to_check);

	SelectivityOptionalFilterState::SelectivityStats min_max_stats(min_max_vectors_to_check, min_max_threshold);
	for (idx_t i = 0; i < min_max_vectors_to_check; i++) {
		min_max_stats.Update(CONTIGUOUS_RANGE_SIZE, DOMAIN_SIZE);
	}
	REQUIRE(min_max_stats.GetSelectivity() == Approx(0.83886));
	REQUIRE(min_max_stats.IsActive());
}

TEST_CASE("String prefix range filter prunes string statistics", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto filter = BuildStringPrefixRangeFilter(*con.context, {"abz", "acz"}, "abz", "acz");

	auto outside = StringStatistics("aaa", StringStatsType::EXACT_STATS, "aaz", StringStatsType::EXACT_STATS);
	REQUIRE(filter->LookupStatistics(outside) == FilterPropagateResult::FILTER_ALWAYS_FALSE);

	auto matching = StringStatistics("abz", StringStatsType::EXACT_STATS, "abz", StringStatsType::EXACT_STATS);
	REQUIRE(filter->LookupStatistics(matching) == FilterPropagateResult::FILTER_ALWAYS_TRUE);

	auto exact_short = StringStatistics("ab", StringStatsType::EXACT_STATS, "ab", StringStatsType::EXACT_STATS);
	REQUIRE(filter->LookupStatistics(exact_short) == FilterPropagateResult::FILTER_ALWAYS_FALSE);

	auto truncated_short =
	    StringStatistics("ab", StringStatsType::TRUNCATED_STATS, "ab", StringStatsType::TRUNCATED_STATS);
	REQUIRE(filter->LookupStatistics(truncated_short) == FilterPropagateResult::NO_PRUNING_POSSIBLE);
}

TEST_CASE("String prefix range statistics handle null bytes and invalid UTF-8", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	const string null_key("ab\0d", 4);
	auto filter = BuildStringPrefixRangeFilter(*con.context, {null_key, "zzzz"}, null_key, "zzzz");

	auto null_stats = StringStatistics(null_key, StringStatsType::EXACT_STATS, null_key, StringStatsType::EXACT_STATS);
	REQUIRE(filter->LookupStatistics(null_stats) == FilterPropagateResult::FILTER_ALWAYS_TRUE);

	auto outside_covering_stats =
	    StringStatistics("aa", StringStatsType::EXACT_STATS, "zzzzzz", StringStatsType::TRUNCATED_STATS);
	REQUIRE(filter->LookupStatistics(outside_covering_stats) == FilterPropagateResult::NO_PRUNING_POSSIBLE);

	auto truncated_null =
	    StringStatistics("ab", StringStatsType::TRUNCATED_STATS, "ab", StringStatsType::TRUNCATED_STATS);
	REQUIRE(filter->LookupStatistics(truncated_null) == FilterPropagateResult::NO_PRUNING_POSSIBLE);

	const string invalid_utf8("\xE6\x97", 2);
	auto invalid_stats = StringStatistics(invalid_utf8, StringStatsType::TRUNCATED_STATS, invalid_utf8,
	                                      StringStatsType::TRUNCATED_STATS);
	REQUIRE(filter->LookupStatistics(invalid_stats) == FilterPropagateResult::FILTER_ALWAYS_FALSE);
}
