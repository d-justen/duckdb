#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/row/tuple_data_layout.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/filter/prefix_range_intervals.hpp"
#include <random>
#include <thread>
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

using namespace duckdb;

namespace {

unique_ptr<PrefixRangeFilter> BuildInt32PrefixRangeFilter(ClientContext &context, const vector<int32_t> &keys,
                                                          int32_t min, int32_t max, idx_t shift,
                                                          double max_false_positive_rate, bool compress = true,
                                                          double estimated_multiplicity = 1.0,
                                                          idx_t parallel_tasks = 0) {
	auto filter = PrefixRangeFilter::CreatePrefixRangeFilter(LogicalType::INTEGER);
	PrefixRangeFilter::Sizing sizing;
	REQUIRE(PrefixRangeFilter::TryComputeSpan(Value::INTEGER(min), Value::INTEGER(max), sizing.span));
	sizing.shift = shift;
	filter->Initialize(context, keys.size(), Value::INTEGER(min), Value::INTEGER(max), sizing, estimated_multiplicity);

	auto state = filter->InitializeBuildState(context);
	Vector key_vector(LogicalType::INTEGER, keys.size());
	auto key_data = FlatVector::GetDataMutable<int32_t>(key_vector);
	for (idx_t i = 0; i < keys.size(); i++) {
		key_data[i] = keys[i];
	}
	FlatVector::SetSize(key_vector, count_t(keys.size()));
	filter->InsertKeys(key_vector, keys.size(), *state);
	filter->MergeBuildState(*state);
	if (compress) {
		if (parallel_tasks) {
			auto compression = filter->InitializeParallelCompression(context, max_false_positive_rate, parallel_tasks);
			REQUIRE(compression);
			do {
				vector<std::thread> workers;
				for (idx_t task_idx = 0; task_idx < compression->TaskCount(); task_idx++) {
					workers.emplace_back([&compression, task_idx]() { compression->ExecuteTask(task_idx); });
				}
				for (auto &worker : workers) {
					worker.join();
				}
			} while (compression->FinishPass());
		} else {
			filter->Compress(context, max_false_positive_rate);
		}
	}
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

TEST_CASE("Parallel prefix range compression matches serial across task boundaries", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);
	const int32_t maximum = (1 << 26) - 59; // Partial final words, with 8 MiB of bitmap storage.
	vector<vector<int32_t>> key_sets = {{0, 1, 2, 16777214, 16777215, 16777216, 16777217, 33554431, maximum},
	                                    {16777214, 16777215, 16777216, 16777217, 16777218, maximum},
	                                    {0, 16777215, 16777217, 33554431, 33554433, 50331647, maximum}};
	for (const auto &keys : key_sets) {
		// Keep this fixture on the dense/parallel path even when the distinct key set is sparse.
		vector<int32_t> build_keys;
		for (idx_t repeat = 0; repeat < 2048; repeat++) {
			build_keys.insert(build_keys.end(), keys.begin(), keys.end());
		}
		for (const auto threshold : {1.0, 0.001, 0.000001}) {
			auto serial = BuildInt32PrefixRangeFilter(*con.context, build_keys, 0, maximum, 0, threshold, true, 2048.0);
			auto parallel =
			    BuildInt32PrefixRangeFilter(*con.context, build_keys, 0, maximum, 0, threshold, true, 2048.0, 4);
			const auto serial_info = serial->GetCompressionInfo();
			const auto parallel_info = parallel->GetCompressionInfo();
			REQUIRE(parallel_info.mode == serial_info.mode);
			REQUIRE(parallel_info.shift == serial_info.shift);
			REQUIRE(parallel_info.range_count == serial_info.range_count);
			REQUIRE(parallel_info.active_buckets == serial_info.active_buckets);
			REQUIRE(parallel_info.run_count == serial_info.run_count);
			REQUIRE(parallel_info.logical_bucket_count == serial_info.logical_bucket_count);
			REQUIRE(parallel_info.range_index_count == serial_info.range_index_count);
			REQUIRE(parallel_info.false_positive_rate == Approx(serial_info.false_positive_rate));
			for (const auto key : keys) {
				REQUIRE(ContainsKey(*parallel, key));
			}
			for (int32_t key = 0; key <= maximum; key += 12731) {
				REQUIRE(ContainsKey(*parallel, key) == ContainsKey(*serial, key));
			}
			for (const int32_t begin : {0, 16777210, maximum - 63}) {
				const auto range = Int32Statistics(begin, MinValue<int32_t>(begin + 63, maximum));
				REQUIRE(parallel->LookupStatistics(range) == serial->LookupStatistics(range));
			}
		}
	}
	vector<int32_t> duplicate_keys;
	for (idx_t repeat = 0; repeat < 2048; repeat++) {
		duplicate_keys.insert(duplicate_keys.end(), {0, 16, 33554432, maximum});
	}
	auto serial = BuildInt32PrefixRangeFilter(*con.context, duplicate_keys, 0, maximum, 1, 0.000001, true, 2048.0);
	auto parallel = BuildInt32PrefixRangeFilter(*con.context, duplicate_keys, 0, maximum, 1, 0.000001, true, 2048.0, 4);
	REQUIRE(parallel->GetCompressionInfo().shift == serial->GetCompressionInfo().shift);
	REQUIRE(parallel->GetCompressionInfo().false_positive_rate ==
	        Approx(serial->GetCompressionInfo().false_positive_rate));
	for (const auto key : duplicate_keys) {
		REQUIRE(ContainsKey(*parallel, key));
	}
}

TEST_CASE("Prefix range filter can retain its original uncompressed bitmap", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto filter = BuildInt32PrefixRangeFilter(*con.context, {100, 101, 102, 103}, 100, 199, 0, 0.001, false);
	const auto info = filter->GetCompressionInfo();

	REQUIRE(info.mode == CompressionMode::BITMAP);
	REQUIRE(info.shift == 0);
	REQUIRE(info.logical_bucket_count == 100);
	REQUIRE(info.bitmap_allocation_bytes > 0);
	REQUIRE(info.range_index_count == 0);
	REQUIRE(ContainsKey(*filter, 100));
	REQUIRE(ContainsKey(*filter, 103));
	REQUIRE(!ContainsKey(*filter, 104));
	REQUIRE(filter->LookupStatistics(Int32Statistics(100, 103)) == FilterPropagateResult::FILTER_ALWAYS_TRUE);
	REQUIRE(filter->LookupStatistics(Int32Statistics(104, 199)) == FilterPropagateResult::FILTER_ALWAYS_FALSE);
}

TEST_CASE("Uncompressed prefix range filter retains the fixed-size initial bitmap", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	static constexpr idx_t MAX_BUCKET_COUNT = 1 << 26;
	PrefixRangeFilter::Sizing sizing;
	REQUIRE(PrefixRangeFilter::TryComputeFixedSizeSizing(Value::INTEGER(0), Value::INTEGER(1 << 28), MAX_BUCKET_COUNT,
	                                                     sizing));
	REQUIRE(sizing.shift > 0);

	auto filter =
	    BuildInt32PrefixRangeFilter(*con.context, {0, 1 << 27, 1 << 28}, 0, 1 << 28, sizing.shift, 0.001, false);
	const auto info = filter->GetCompressionInfo();
	REQUIRE(info.mode == CompressionMode::BITMAP);
	REQUIRE(info.shift == sizing.shift);
	REQUIRE(info.logical_bucket_count <= MAX_BUCKET_COUNT);
	REQUIRE(info.range_index_count == 0);
	REQUIRE(ContainsKey(*filter, 0));
	REQUIRE(ContainsKey(*filter, 1 << 27));
	REQUIRE(ContainsKey(*filter, 1 << 28));
}

TEST_CASE("Prefix range filter common build merges every bitmap bucket before postprocessing", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);
	vector<int32_t> keys {0, 63, 64, 65, 511, 512, 4095};
	for (int32_t key = 100; key < 300; key++) {
		keys.push_back(key);
		keys.push_back(key); // Duplicates can be distributed across different local states.
	}
	TupleDataLayout layout;
	layout.Initialize({LogicalType::INTEGER}, TupleDataValidityType::CAN_HAVE_NULL_VALUES);
	vector<data_t> storage((keys.size() + 1) * layout.GetRowWidth(), 0);
	vector<data_ptr_t> rows;
	for (idx_t i = 0; i < keys.size(); i++) {
		auto row = storage.data() + i * layout.GetRowWidth();
		row[0] = 1; // The first column's validity bit in the tuple-data header.
		Store<int32_t>(keys[i], row + layout.GetOffsets()[0]);
		rows.push_back(row);
	}
	// A NULL row must not set the bucket of its otherwise valid payload.
	auto null_row = storage.data() + keys.size() * layout.GetRowWidth();
	Store<int32_t>(4000, null_row + layout.GetOffsets()[0]);
	rows.push_back(null_row);
	for (idx_t shift : {0, 3}) {
		auto serial = BuildInt32PrefixRangeFilter(*con.context, keys, 0, 4095, shift, 0.001, false);
		for (idx_t tasks : {1, 4}) {
			auto filter = PrefixRangeFilter::CreatePrefixRangeFilter(LogicalType::INTEGER);
			PrefixRangeFilter::Sizing sizing;
			REQUIRE(PrefixRangeFilter::TryComputeSpan(Value::INTEGER(0), Value::INTEGER(4095), sizing.span));
			sizing.shift = shift;
			filter->Initialize(*con.context, keys.size(), Value::INTEGER(0), Value::INTEGER(4095), sizing);
			vector<unique_ptr<PrefixRangeFilter::BuildState>> states;
			for (idx_t task = 0; task < tasks; task++) {
				auto state = filter->InitializeBuildState(*con.context, tasks == 1);
				REQUIRE((state->bitmap_allocation_bytes == 0) == (tasks == 1));
				vector<data_ptr_t> task_rows;
				for (idx_t i = task; i < rows.size(); i += tasks) {
					task_rows.push_back(rows[i]);
				}
				filter->InsertRows(task_rows.data(), task_rows.size(), layout, *state);
				states.push_back(std::move(state));
			}
			for (auto &state : states) {
				filter->MergeBuildState(*state);
			}
			// Neither compression nor analysis has run. Looking up every bucket compares the initial bitmaps.
			for (int32_t key = 0; key <= 4095; key++) {
				REQUIRE(ContainsKey(*filter, key) == ContainsKey(*serial, key));
			}
			const auto before = filter->GetCompressionInfo(false);
			filter->Analyze();
			const auto after = filter->GetCompressionInfo(false);
			REQUIRE(before.mode == CompressionMode::BITMAP);
			REQUIRE(after.mode == before.mode);
			REQUIRE(after.shift == before.shift);
			REQUIRE(after.bitmap_allocation_bytes == before.bitmap_allocation_bytes);
		}
	}
}

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

TEST_CASE("Prefix range filter uses estimated multiplicity for its FPR decision", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	const vector<int32_t> keys = {0, 1, 2, 3, 512, 513, 514, 515};
	static constexpr double MAX_FALSE_POSITIVE_RATE = 0.001;
	auto conservative = BuildInt32PrefixRangeFilter(*con.context, keys, 0, 1023, 2, MAX_FALSE_POSITIVE_RATE, true, 4.0);
	auto estimated = BuildInt32PrefixRangeFilter(*con.context, keys, 0, 1023, 2, MAX_FALSE_POSITIVE_RATE);

	REQUIRE(conservative->Analyze().active_buckets == 2);
	REQUIRE(conservative->Analyze().false_positive_rate > MAX_FALSE_POSITIVE_RATE);
	REQUIRE(estimated->Analyze().active_buckets == 2);
	REQUIRE(estimated->Analyze().false_positive_rate == Approx(0.0));
	REQUIRE(estimated->GetCompressionInfo().false_positive_rate == Approx(0.0));
	for (const auto key : keys) {
		REQUIRE(ContainsKey(*estimated, key));
	}
	REQUIRE(!ContainsKey(*estimated, 4));

	const vector<int32_t> dyadic_keys = {0, 1, 4, 5, 8, 9, 12, 13, 16, 17};
	auto conservative_dyadic = BuildInt32PrefixRangeFilter(*con.context, dyadic_keys, 0, 127, 1, 0.09, true, 2.0);
	auto estimated_dyadic = BuildInt32PrefixRangeFilter(*con.context, dyadic_keys, 0, 127, 1, 0.09);
	REQUIRE(conservative_dyadic->GetCompressionInfo().mode == CompressionMode::BITMAP);
	REQUIRE(estimated_dyadic->GetCompressionInfo().mode == CompressionMode::DIRECT_RANGES);
	REQUIRE(estimated_dyadic->GetCompressionInfo().false_positive_rate <= 0.09);
	for (const auto key : dyadic_keys) {
		REQUIRE(ContainsKey(*estimated_dyadic, key));
	}
}

TEST_CASE("Prefix range filter applies fractional multiplicity to initial FPR", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);
	const vector<int32_t> keys = {0, 0, 16};
	auto filter = BuildInt32PrefixRangeFilter(*con.context, keys, 0, 64, 4, 0.47, false, 1.5);
	const auto analysis = filter->Analyze();
	REQUIRE(analysis.active_buckets == 2);
	REQUIRE(analysis.false_positive_rate == Approx(30.0 / 63.0));
	REQUIRE(filter->GetCompressionInfo().false_positive_rate == Approx(analysis.false_positive_rate));
	filter->Compress(*con.context, 0.47);
	REQUIRE(filter->GetCompressionInfo().mode == CompressionMode::BITMAP);
	REQUIRE(filter->Analyze().false_positive_rate == Approx(analysis.false_positive_rate));

	auto no_stats = BuildInt32PrefixRangeFilter(*con.context, keys, 0, 64, 4, 0.47, false);
	REQUIRE(no_stats->Analyze().false_positive_rate == Approx(29.0 / 62.0));
	no_stats->Compress(*con.context, 0.47);
	REQUIRE(no_stats->GetCompressionInfo().mode == CompressionMode::DIRECT_RANGES);
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
		keys.push_back(key + 1);
	}
	auto filter = BuildInt32PrefixRangeFilter(*con.context, keys, 0, 262141, 0, 1.0);
	auto info = filter->GetCompressionInfo();
	const auto first_analysis = filter->Analyze();
	const auto second_analysis = filter->Compress(*con.context, 1.0);
	const auto repeated_info = filter->GetCompressionInfo();

	REQUIRE(info.mode == CompressionMode::BITMAP);
	REQUIRE(info.shift == 1);
	REQUIRE(info.logical_bucket_count == 131071);
	REQUIRE(info.bitmap_allocation_bytes == 64 + 2048 * sizeof(uint64_t));
	REQUIRE(info.run_count == keys.size() / 2);
	REQUIRE(first_analysis.active_buckets == info.active_buckets);
	REQUIRE(first_analysis.false_positive_rate == Approx(info.false_positive_rate));
	REQUIRE(second_analysis.active_buckets == first_analysis.active_buckets);
	REQUIRE(second_analysis.false_positive_rate == Approx(first_analysis.false_positive_rate));
	REQUIRE(repeated_info.shift == info.shift);
	REQUIRE(repeated_info.bitmap_allocation_bytes == info.bitmap_allocation_bytes);
	REQUIRE(ContainsKey(*filter, keys.front()));
	REQUIRE(ContainsKey(*filter, keys.back()));
}

TEST_CASE("Prefix range filter uses source multiplicity for duplicate build keys", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<int32_t> keys;
	for (idx_t repeat = 0; repeat < 100; repeat++) {
		for (int32_t key : {0, 16, 32, 48}) {
			keys.push_back(key);
		}
	}

	auto filter = BuildInt32PrefixRangeFilter(*con.context, keys, 0, 64, 4, 1.0, true, 100.0);
	auto analysis = filter->Analyze();
	REQUIRE(analysis.active_buckets == 4);
	REQUIRE(analysis.false_positive_rate > 0.9);
	auto no_stats = BuildInt32PrefixRangeFilter(*con.context, keys, 0, 64, 4, 1.0, false);
	REQUIRE(no_stats->Analyze().false_positive_rate == Approx(0.0));
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

TEST_CASE("Prefix range sparse compression agrees with dense compression and range pruning", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);
	const int32_t maximum = (1 << 26) - 59;
	for (const vector<int32_t> keys : {vector<int32_t> {0, 1, 2, 63, 64, 8191, 8192, 16777215, 16777216, maximum},
	                                   vector<int32_t> {0, 1, 2, 3, 100000, 100001, 100002, 100003}}) {
		vector<int32_t> repeated;
		for (idx_t i = 0; i < 1024; i++) {
			repeated.insert(repeated.end(), keys.begin(), keys.end());
		}
		for (idx_t shift : {0, 1, 4}) {
			for (double fpr : {0.0, 0.001, 1.0}) {
				auto sparse = BuildInt32PrefixRangeFilter(*con.context, keys, 0, maximum, shift, fpr);
				auto dense = BuildInt32PrefixRangeFilter(*con.context, repeated, 0, maximum, shift, fpr, true, 1024.0);
				const auto a = sparse->GetCompressionInfo();
				const auto b = dense->GetCompressionInfo();
				REQUIRE(a.mode == b.mode);
				REQUIRE(a.shift == b.shift);
				REQUIRE(a.active_buckets == b.active_buckets);
				REQUIRE(a.run_count == b.run_count);
				REQUIRE(a.false_positive_rate == Approx(b.false_positive_rate));
				REQUIRE(a.range_index_count == b.range_index_count);
				for (const auto key : keys) {
					REQUIRE(ContainsKey(*sparse, key));
					for (int32_t offset : {-1, 0, 1}) {
						REQUIRE(ContainsKey(*sparse, key + offset) == ContainsKey(*dense, key + offset));
					}
				}
				for (int32_t begin = 0; begin < maximum - 300000; begin += 123457) {
					const auto stats = Int32Statistics(begin, begin + 300000);
					REQUIRE(sparse->LookupStatistics(stats) == dense->LookupStatistics(stats));
				}
			}
		}
	}
}

TEST_CASE("Prefix range compression limits expansion of highly selective bitmaps", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);
	vector<int32_t> keys;
	for (int32_t key = 0; key < (1 << 22); key += 65535) {
		keys.push_back(key);
	}
	for (idx_t repeats : {1, 128}) {
		vector<int32_t> build;
		for (idx_t i = 0; i < repeats; i++) {
			build.insert(build.end(), keys.begin(), keys.end());
		}
		auto filter =
		    BuildInt32PrefixRangeFilter(*con.context, build, 0, 1 << 22, 0, 0.001, true, static_cast<double>(repeats));
		REQUIRE(filter->GetCompressionInfo().shift == 0);
		REQUIRE(filter->Analyze().false_positive_rate == 0);
		for (const auto key : keys) {
			REQUIRE(ContainsKey(*filter, key));
			REQUIRE(!ContainsKey(*filter, key + 1));
		}
	}
}

TEST_CASE("Prefix range compression preflight preserves partial words and exact analysis", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);
	const int32_t maximum = (1 << 26) - 2;
	const vector<int32_t> keys = {0, 32, 64, 96, 128, 160, maximum};
	vector<int32_t> repeated;
	for (idx_t repeat = 0; repeat < 1024; repeat++) {
		repeated.insert(repeated.end(), keys.begin(), keys.end());
	}
	for (idx_t shift : {0, 3}) {
		for (double fpr : {0.0, 0.001}) {
			for (bool parallel : {false, true}) {
				auto filter =
				    BuildInt32PrefixRangeFilter(*con.context, repeated, 0, maximum, shift, fpr, false, 1024.0);
				const auto initial = filter->Analyze();
				if (parallel) {
					// The large bitmap is finalized by the preflight; the smaller one uses serial compression.
					REQUIRE(!filter->InitializeParallelCompression(*con.context, fpr, 4));
				}
				const auto result = filter->Compress(*con.context, fpr);
				const auto info = filter->GetCompressionInfo();
				REQUIRE(info.shift == shift);
				REQUIRE(info.run_count == keys.size());
				REQUIRE(result.active_buckets == initial.active_buckets);
				REQUIRE(result.false_positive_rate == Approx(initial.false_positive_rate));
				for (const auto key : keys) {
					REQUIRE(ContainsKey(*filter, key));
				}
				REQUIRE(!ContainsKey(*filter, 24));
				REQUIRE(!ContainsKey(*filter, maximum - 8));
				if (result.false_positive_rate <= fpr) {
					REQUIRE(filter->LookupStatistics(Int32Statistics(1024, maximum - 1024)) ==
					        FilterPropagateResult::FILTER_ALWAYS_FALSE);
				}
			}
		}
	}
}

TEST_CASE("Prefix range homogeneous blocks preserve exact boundary membership", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);
	vector<int32_t> keys;
	for (int32_t key = 1; key < 32767; key++) {
		keys.push_back(key);
	}
	for (int32_t key : {65535, 65536, 98305, 131071, 131100}) {
		keys.push_back(key);
	}
	const auto filter = BuildInt32PrefixRangeFilter(*con.context, keys, 0, 131100, 0, 0.0);
	REQUIRE(filter->GetCompressionInfo().active_buckets == keys.size());
	idx_t pos = 0;
	for (int32_t key = 0; key <= 131100; key++) {
		const bool present = pos < keys.size() && keys[pos] == key;
		REQUIRE(ContainsKey(*filter, key) == present);
		pos += present;
	}
	REQUIRE(filter->LookupStatistics(Int32Statistics(64, 32703)) == FilterPropagateResult::FILTER_ALWAYS_TRUE);
	REQUIRE(filter->LookupStatistics(Int32Statistics(32768, 65534)) == FilterPropagateResult::FILTER_ALWAYS_FALSE);
}

TEST_CASE("Prefix range sparse and dense compression bound cumulative expansion", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);
	vector<int32_t> keys;
	for (int32_t cluster = 0; cluster < 6; cluster++) {
		for (int32_t i = 0; i < 33; i++) {
			keys.push_back(cluster * 524288 + i);
		}
	}
	// First reduction covers 34 values per cluster (3% expansion). The next would cover
	// 36 values (9% cumulative expansion), even though its incremental expansion is <6.25%.
	for (idx_t repeats : {1, 64}) {
		vector<int32_t> build;
		for (idx_t i = 0; i < repeats; i++) {
			build.insert(build.end(), keys.begin(), keys.end());
		}
		auto filter =
		    BuildInt32PrefixRangeFilter(*con.context, build, 0, 1 << 22, 0, 0.001, true, static_cast<double>(repeats));
		const auto info = filter->GetCompressionInfo();
		REQUIRE(info.mode == CompressionMode::BITMAP);
		REQUIRE(info.shift == 1);
		REQUIRE(info.active_buckets == 6 * 17);
		REQUIRE(info.bitmap_allocation_bytes == 64 + (((1 << 21) + 1 + 63) / 64) * sizeof(uint64_t));
		for (int32_t cluster = 0; cluster < 6; cluster++) {
			const auto base = cluster * 524288;
			for (int32_t i = 0; i <= 33; i++) {
				REQUIRE(ContainsKey(*filter, base + i));
			}
			REQUIRE(!ContainsKey(*filter, base + 34));
			REQUIRE(filter->LookupStatistics(Int32Statistics(base + 64, base + 500000)) ==
			        FilterPropagateResult::FILTER_ALWAYS_FALSE);
		}
	}
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

TEST_CASE("Interval scratch reduction materializes the exact dyadic bitmap", "[optimizer][prf]") {
	std::mt19937_64 random(42);
	for (idx_t buckets : {idx_t(1), idx_t(63), idx_t(64), idx_t(65), idx_t(8193), idx_t(262147)}) {
		for (idx_t pattern = 0; pattern < 5; pattern++) {
			vector<uint64_t> source((buckets + 63) / 64, 0);
			for (idx_t bit = 0; bit < buckets; bit++) {
				const bool set = pattern == 0 || (pattern == 1 && (bit / 131) % 3 == 0) ||
				                 (pattern == 2 && bit % 257 == 0) || (pattern == 3 && random() % 7 == 0);
				if (set) {
					source[bit / 64] |= uint64_t(1) << (bit % 64);
				}
			}
			for (bool blocked : {false, true}) {
				vector<PrefixRangeInterval> intervals;
				REQUIRE((blocked
				             ? PrefixRangeIntervals::Collect<true>(source.data(), buckets, 0, source.size(),
				                                                   PrefixRangeIntervals::MAX_INTERVALS, intervals)
				             : PrefixRangeIntervals::Collect<false>(source.data(), buckets, 0, source.size(),
				                                                    PrefixRangeIntervals::MAX_INTERVALS, intervals)));
				auto reference = source;
				auto logical = buckets;
				for (idx_t level = 0; level < 8; level++) {
					vector<uint64_t> materialized(reference.size(), ~uint64_t(0));
					// Split through runs and at non-block boundaries; each task owns entire words.
					for (idx_t begin = 0; begin < reference.size(); begin += 37) {
						PrefixRangeIntervals::Materialize(intervals, materialized.data(), begin,
						                                  MinValue<idx_t>(begin + 37, reference.size()));
					}
					REQUIRE(materialized == reference);
					if (logical == 1) {
						break;
					}
					vector<PrefixRangeInterval> reduced;
					PrefixRangeIntervals::Reduce(intervals, reduced);
					intervals.swap(reduced);
					vector<uint64_t> next(((logical + 1) / 2 + 63) / 64, 0);
					for (idx_t bit = 0; bit < logical; bit++) {
						if ((reference[bit / 64] >> (bit % 64)) & 1) {
							next[(bit / 2) / 64] |= uint64_t(1) << ((bit / 2) % 64);
						}
					}
					reference.swap(next);
					logical = (logical + 1) / 2;
				}
			}
		}
	}
}

TEST_CASE("Interval collection bounds memory and masks unused tail bits", "[optimizer][prf]") {
	vector<uint64_t> source(4097, 0x5555555555555555ULL);
	const auto original = source;
	for (bool blocked : {false, true}) {
		vector<PrefixRangeInterval> intervals;
		const auto complete = blocked ? PrefixRangeIntervals::Collect<true>(source.data(), source.size() * 64, 0,
		                                                                    source.size(), 65536, intervals)
		                              : PrefixRangeIntervals::Collect<false>(source.data(), source.size() * 64, 0,
		                                                                     source.size(), 65536, intervals);
		REQUIRE_FALSE(complete);
		REQUIRE(intervals.size() == 65536);
		REQUIRE(intervals.capacity() <= 65536);
		REQUIRE(source == original);
	}
	vector<uint64_t> tail(129, ~uint64_t(0));
	vector<PrefixRangeInterval> intervals;
	REQUIRE(PrefixRangeIntervals::Collect<true>(tail.data(), 8193, 0, tail.size(), 65536, intervals));
	REQUIRE(intervals.size() == 1);
	REQUIRE(intervals[0].lower == 0);
	REQUIRE(intervals[0].upper == 8192);
}

TEST_CASE("Parallel interval overflow preserves the bitmap and falls back", "[optimizer][prf]") {
	DuckDB db(nullptr);
	Connection con(db);
	vector<int32_t> keys;
	for (int32_t key = 0; key < 10000; key++) {
		keys.push_back(key);
	}
	// More intervals than the global budget, concentrated in the first partition.
	for (int32_t i = 0; i < 70000; i++) {
		keys.push_back(16384 + i * 64);
	}
	const int32_t maximum = (1 << 26) - 7;
	auto serial = BuildInt32PrefixRangeFilter(*con.context, keys, 0, maximum, 0, 0.0);
	auto parallel = BuildInt32PrefixRangeFilter(*con.context, keys, 0, maximum, 0, 0.0, true, 1.0, 8);
	REQUIRE(serial->GetCompressionInfo().shift == 0);
	REQUIRE(parallel->GetCompressionInfo().shift == 0);
	REQUIRE(parallel->Analyze().active_buckets == keys.size());
	for (auto key : keys) {
		REQUIRE(ContainsKey(*parallel, key));
		if (key >= 16384) {
			REQUIRE_FALSE(ContainsKey(*parallel, key + 1));
		}
	}
}

TEST_CASE("Interval reductions match every covered bucket", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);
	const int32_t maximum = 262147;
	for (idx_t clusters : {idx_t(5), idx_t(16), idx_t(256), idx_t(4096)}) {
		vector<int32_t> keys;
		for (idx_t c = 0; c < clusters; c++) {
			const auto begin = c * 262100 / clusters;
			const auto length = MaxValue<idx_t>(1, 8192 / clusters);
			for (idx_t i = 0; i < length; i++) {
				keys.push_back(begin + i);
			}
		}
		for (double threshold : {0.0, 0.001, 1.0}) {
			auto filter = BuildInt32PrefixRangeFilter(*con.context, keys, 0, maximum, 0, threshold);
			const auto info = filter->GetCompressionInfo();
			vector<bool> buckets((maximum >> info.shift) + 1, false);
			for (auto key : keys) {
				buckets[key >> info.shift] = true;
			}
			for (int32_t key = 0; key <= maximum; key++) {
				REQUIRE(ContainsKey(*filter, key) == buckets[key >> info.shift]);
			}
		}
	}
}
