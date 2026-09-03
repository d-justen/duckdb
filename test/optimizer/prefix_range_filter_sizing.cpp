#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/planner/filter/prefix_range_filter.hpp"

using namespace duckdb;

namespace {

unique_ptr<PrefixRangeFilter> BuildInt32PrefixRangeFilter(ClientContext &context, const vector<int32_t> &keys,
                                                          int32_t min, int32_t max, idx_t shift) {
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
	filter->InsertKeys(key_vector, *state);
	filter->MergeBuildState(*state);
	return filter;
}

} // namespace

TEST_CASE("Prefix range filter computes fixed-size bitmap sizing", "[optimizer][prefix_range_filter]") {
	PrefixRangeFilter::Sizing sizing;
	idx_t bucket_count;

	REQUIRE(PrefixRangeFilter::TryComputeFixedSizeSizing(Value::INTEGER(0), Value::INTEGER(1023), 1024, sizing));
	REQUIRE(sizing.span == 1023);
	REQUIRE(sizing.shift == 0);
	REQUIRE(PrefixRangeFilter::TryComputeBucketCount(sizing.span, sizing.shift, bucket_count));
	REQUIRE(bucket_count == 1024);

	REQUIRE(PrefixRangeFilter::TryComputeFixedSizeSizing(Value::INTEGER(0), Value::INTEGER(1023), 512, sizing));
	REQUIRE(sizing.shift == 1);
	REQUIRE(PrefixRangeFilter::TryComputeBucketCount(sizing.span, sizing.shift, bucket_count));
	REQUIRE(bucket_count == 512);

	REQUIRE(PrefixRangeFilter::TryComputeFixedSizeSizing(Value::INTEGER(7), Value::INTEGER(7), 1, sizing));
	REQUIRE(sizing.span == 0);
	REQUIRE(sizing.shift == 0);
	REQUIRE(PrefixRangeFilter::TryComputeBucketCount(sizing.span, sizing.shift, bucket_count));
	REQUIRE(bucket_count == 1);

	const auto max_uint64 = NumericLimits<uint64_t>::Maximum();
	REQUIRE(PrefixRangeFilter::TryComputeFixedSizeSizing(Value::UBIGINT(0), Value::UBIGINT(max_uint64), 2, sizing));
	REQUIRE(sizing.span == Uhugeint::Convert(max_uint64));
	REQUIRE(sizing.shift == 63);
	REQUIRE(PrefixRangeFilter::TryComputeBucketCount(sizing.span, sizing.shift, bucket_count));
	REQUIRE(bucket_count == 2);
	REQUIRE_FALSE(
	    PrefixRangeFilter::TryComputeFixedSizeSizing(Value::UBIGINT(0), Value::UBIGINT(max_uint64), 1, sizing));
	REQUIRE_FALSE(PrefixRangeFilter::TryComputeBucketCount(sizing.span, 64, bucket_count));

	REQUIRE_FALSE(PrefixRangeFilter::TryComputeFixedSizeSizing(Value::INTEGER(0), Value::INTEGER(10), 0, sizing));
}

TEST_CASE("Prefix range filter sizing respects its false-positive budget", "[optimizer][prefix_range_filter]") {
	PrefixRangeFilter::Sizing sizing;

	REQUIRE(PrefixRangeFilter::TryComputeSizing(Value::INTEGER(0), Value::INTEGER(1023), 128, sizing, 0.15));
	REQUIRE(sizing.shift == 1);
	REQUIRE(PrefixRangeFilter::ComputeFalsePositiveRateUpperBound(sizing.span, 128, sizing.shift) == Approx(1.0 / 7.0));

	REQUIRE(PrefixRangeFilter::TryComputeSizing(Value::INTEGER(0), Value::INTEGER(1023), 128, sizing, 0.1));
	REQUIRE(sizing.shift == 0);
	REQUIRE(PrefixRangeFilter::ComputeFalsePositiveRateUpperBound(sizing.span, 128, sizing.shift) == 0);

	REQUIRE_FALSE(PrefixRangeFilter::TryComputeSizing(Value::INTEGER(0), Value::INTEGER(1023), 0, sizing));
	REQUIRE_FALSE(PrefixRangeFilter::TryComputeSizing(Value::INTEGER(0), Value::INTEGER(1023), 128, sizing, -1));
}

TEST_CASE("Prefix range filter analysis uses active buckets as its positive lower bound",
          "[optimizer][prefix_range_filter]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<int32_t> keys;
	for (idx_t repeat = 0; repeat < 100; repeat++) {
		for (int32_t key : {0, 16, 32, 48}) {
			keys.push_back(key);
		}
	}

	auto filter = BuildInt32PrefixRangeFilter(*con.context, keys, 0, 64, 4);
	const auto analysis = filter->Analyze();
	REQUIRE(analysis.active_buckets == 4);
	REQUIRE(analysis.false_positive_rate == Approx(60.0 / 61.0));
}
