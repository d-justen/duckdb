//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/filter/table_filter_prefix_range_function.cpp
//
//
//===----------------------------------------------------------------------===//

#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/filter/table_filter_function_helpers.hpp"

#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

#include <algorithm>

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/array.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/enums/filter_propagate_result.hpp"
#include "duckdb/common/enums/vector_type.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/planner/table_filter_state.hpp"

namespace duckdb {

AllocatedData AllocateBitmap(ClientContext &context, const idx_t word_count, uint64_t *&bitmap_begin) {
	const idx_t size = word_count * sizeof(uint64_t);
	BufferManager &buffer_manager = BufferManager::GetBufferManager(context);
	auto buffer = buffer_manager.GetBufferAllocator().Allocate(64ULL + size);
	bitmap_begin = reinterpret_cast<uint64_t *>((64ULL + reinterpret_cast<uint64_t>(buffer.get())) & ~63ULL);
	std::fill_n(bitmap_begin, word_count, 0);
	return buffer;
}

struct PrefixRangeBitmapBuildState : public PrefixRangeFilter::BuildState {
	explicit PrefixRangeBitmapBuildState(AllocatedData data_p, uint64_t *bitmap_p)
	    : data(std::move(data_p)), bitmap(bitmap_p) {
	}

	AllocatedData data;
	uint64_t *bitmap;
};

template <typename U>
class PrefixRangeBitmap {
public:
	void Initialize(ClientContext &context, U min_p, U span_p, idx_t shift_p) {
		min = min_p;
		span = span_p;
		shift = shift_p;

		const idx_t buckets = UnsafeNumericCast<idx_t>((span >> shift) + 1);
		logical_bucket_count = buckets;
		word_count = buckets == 0 ? 1 : (buckets + 63) >> WORD_SHIFT;

		buf_ = AllocateBitmap(context, word_count, bitmap);
		mode = Mode::BITMAP;
		range_count = 0;
		base_active_buckets = 0;
		current_active_buckets = 0;
		current_run_count = 0;
		current_run_count_is_exact = false;
		cached_false_positive_rate = 0;
		analysis_cached = false;
		compression_finalized = false;

		// Only mark initialized as true when local bitmaps are merged.
		initialized = false;
	}

	unique_ptr<PrefixRangeBitmapBuildState> InitializeBuildState(ClientContext &context) const {
		D_ASSERT(bitmap);
		uint64_t *state_bitmap;
		auto state_data = AllocateBitmap(context, word_count, state_bitmap);
		return make_uniq<PrefixRangeBitmapBuildState>(std::move(state_data), state_bitmap);
	}

	template <typename T, typename CONVERTER>
	void InsertKeys(Vector &keys, idx_t count, uint64_t *state_bitmap) const {
		for (const auto &entry : keys.template ValidValues<T>()) {
			const U y = CONVERTER::Convert(entry.GetValue()) - min;
			// All keys are in-range by construction, so the range check can be omitted here.
			const U idx = y >> shift;
			state_bitmap[idx >> WORD_SHIFT] |= 1ULL << (idx & WORD_MASK);
		}
	}

	void MergeBuildState(PrefixRangeBitmapBuildState &state) {
		for (idx_t word_idx = 0; word_idx < word_count; word_idx++) {
			bitmap[word_idx] |= state.bitmap[word_idx];
		}
		initialized = true;
		analysis_cached = false;
		compression_finalized = false;
	}

	template <typename T, typename CONVERTER>
	inline bool LookupOne(const Value &value) const {
		if (value.IsNull()) {
			return false;
		}

		const U comparable = CONVERTER::Convert(value.GetValueUnsafe<T>());
		if (mode == Mode::DIRECT_RANGES) {
			return DirectRangeLookup(comparable);
		}
		const U y = comparable - min;
		const uint8_t in_range = y <= span;
		const U bit_idx = ShiftRight(y, shift);
		const uint32_t word_idx = (bit_idx >> WORD_SHIFT) & (0U - in_range);
		const uint8_t bit = (bitmap[word_idx] >> (bit_idx & WORD_MASK)) & 1ULL;
		return bit & in_range;
	}

	template <typename T, typename CONVERTER>
	idx_t LookupKeys(Vector &keys, SelectionVector &result_sel, idx_t count) const {
		if (mode == Mode::DIRECT_RANGES) {
			return LookupKeysDirect<T, CONVERTER>(keys, result_sel, count);
		}
		return LookupKeysBitmap<T, CONVERTER>(keys, result_sel, count);
	}

	FilterPropagateResult LookupRange(U lower_bound, U upper_bound) const {
		if (mode == Mode::DIRECT_RANGES) {
			auto covered_until = lower_bound;
			bool found_overlap = false;
			for (idx_t range_idx = 0; range_idx < range_count; range_idx++) {
				const auto &range = ranges[range_idx];
				const auto range_upper = DirectRangeUpper(range);
				if (range_upper < lower_bound) {
					continue;
				}
				if (range.lower > upper_bound) {
					break;
				}
				if (!found_overlap) {
					if (range.lower > lower_bound) {
						return FilterPropagateResult::NO_PRUNING_POSSIBLE;
					}
					found_overlap = true;
				} else if ((covered_until != NumericLimits<U>::Maximum() && range.lower > covered_until + 1) ||
				           (covered_until == NumericLimits<U>::Maximum() && range.lower > covered_until)) {
					return FilterPropagateResult::NO_PRUNING_POSSIBLE;
				}
				if (range_upper >= upper_bound) {
					return FilterPropagateResult::FILTER_ALWAYS_TRUE;
				}
				covered_until = range_upper;
			}
			return found_overlap ? FilterPropagateResult::NO_PRUNING_POSSIBLE
			                     : FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}

		const U lb_y = lower_bound - min;
		const U lb_bit_idx = ShiftRight(lb_y, shift);
		const auto lb_word_idx = lb_bit_idx >> WORD_SHIFT;

		const U ub_y = upper_bound - min;
		const U ub_bit_idx = ShiftRight(ub_y, shift);
		const auto ub_word_idx = ub_bit_idx >> WORD_SHIFT;

		const idx_t lb_bit_off = UnsafeNumericCast<idx_t>(lb_bit_idx & UnsafeNumericCast<U>(WORD_MASK));
		const idx_t ub_bit_off = UnsafeNumericCast<idx_t>(ub_bit_idx & UnsafeNumericCast<U>(WORD_MASK));

		bool any_set = false;
		bool all_set = true;
		if (lb_word_idx == ub_word_idx) {
			const auto range_mask = ((~0ULL << lb_bit_off) & (~0ULL >> (WORD_MASK - ub_bit_off)));
			const auto word = bitmap[lb_word_idx] & range_mask;
			any_set = word != 0;
			all_set = word == range_mask;
		} else {
			const auto lb_word_mask = (~0ULL << lb_bit_off);
			const auto lb_word = bitmap[lb_word_idx] & lb_word_mask;
			any_set |= lb_word != 0;
			all_set &= lb_word == lb_word_mask;

			for (idx_t i = UnsafeNumericCast<idx_t>(lb_word_idx) + 1; i < UnsafeNumericCast<idx_t>(ub_word_idx); i++) {
				const auto word = bitmap[i];
				any_set |= word != 0;
				all_set &= word == ~0ULL;
			}

			const auto ub_word_mask = ~0ULL >> (WORD_MASK - ub_bit_off);
			const auto ub_word = bitmap[ub_word_idx] & ub_word_mask;
			any_set |= ub_word != 0;
			all_set &= ub_word == ub_word_mask;
		}

		if (all_set) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		return any_set ? FilterPropagateResult::NO_PRUNING_POSSIBLE : FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}

	bool IsInitialized() const {
		return initialized;
	}

	PrefixRangeFilter::Analysis Analyze() const {
		if (analysis_cached) {
			return {current_active_buckets, cached_false_positive_rate};
		}

		const auto current_active_buckets = CountActiveBuckets();
		return {current_active_buckets, FalsePositiveRate(current_active_buckets, shift, current_active_buckets)};
	}

	PrefixRangeFilter::Analysis Compress(ClientContext &context, double max_false_positive_rate) {
		if (!initialized || compression_finalized || mode != Mode::BITMAP || max_false_positive_rate < 0) {
			return Analyze();
		}
		CompressBitmap(context, max_false_positive_rate);
		return Analyze();
	}

	PrefixRangeFilter::CompressionInfo GetCompressionInfo() const {
		PrefixRangeFilter::CompressionInfo info;
		info.mode = mode == Mode::DIRECT_RANGES ? CompressionMode::DIRECT_RANGES : CompressionMode::BITMAP;
		info.shift = shift;
		info.range_count = range_count;
		info.active_buckets = analysis_cached ? current_active_buckets : CountActiveBuckets();
		info.run_count = analysis_cached && current_run_count_is_exact ? current_run_count : CountRuns();
		info.logical_bucket_count = logical_bucket_count;
		info.bitmap_allocation_bytes = mode == Mode::BITMAP ? buf_.GetSize() : 0;
		info.false_positive_rate = analysis_cached ? cached_false_positive_rate
		                                           : FalsePositiveRate(info.active_buckets, shift, info.active_buckets);
		return info;
	}

	U Min() const {
		return min;
	}

	U Span() const {
		return span;
	}

private:
	static constexpr idx_t WORD_SHIFT = 6;
	static constexpr idx_t WORD_MASK = 63;
	static constexpr idx_t MAX_DIRECT_RANGES = 4;
	static constexpr idx_t METRICS_BLOCK_WORDS = 64;
	static constexpr idx_t BITMAP_CACHE_TARGET_BYTES = 16 * 1024;

	enum class Mode : uint8_t { BITMAP, DIRECT_RANGES };

	struct DirectRange {
		U lower;
		U width;
	};

	struct BitmapMetrics {
		idx_t active_buckets = 0;
		idx_t run_count = 0;
		idx_t recorded_starts = 0;
		idx_t recorded_ends = 0;
		bool run_count_is_exact = true;
		array<idx_t, MAX_DIRECT_RANGES> run_starts;
		array<idx_t, MAX_DIRECT_RANGES> run_ends;

		bool HasExactRanges() const {
			return run_count_is_exact && run_count > 0 && run_count <= MAX_DIRECT_RANGES &&
			       recorded_starts == run_count && recorded_ends == run_count;
		}
	};

	struct BitmapStorage {
		AllocatedData data;
		uint64_t *bitmap = nullptr;
		idx_t word_capacity = 0;
	};

	struct DyadicPassResult {
		BitmapMetrics source;
		BitmapMetrics destination;
	};

	class BitmapMetricsBuilder {
	public:
		template <bool ANALYZE_RUNS>
		void PushWord(uint64_t word, idx_t valid_bits, idx_t word_base) {
			const auto first_bit = static_cast<uint8_t>(word & 1ULL);
			if (has_pending) {
				ConsumePending<ANALYZE_RUNS>(first_bit);
			}
			pending_word = word;
			pending_valid_bits = valid_bits;
			pending_word_base = word_base;
			has_pending = true;
		}

		template <bool ANALYZE_RUNS>
		BitmapMetrics Finish() {
			if (has_pending) {
				ConsumePending<ANALYZE_RUNS>(0);
				has_pending = false;
			}
			metrics.run_count_is_exact = ANALYZE_RUNS;
			return metrics;
		}

		idx_t RunCount() const {
			return metrics.run_count;
		}

	private:
		void RecordPositions(uint64_t positions, array<idx_t, MAX_DIRECT_RANGES> &result, idx_t &count) {
			while (positions != 0 && count < MAX_DIRECT_RANGES) {
				const auto bit = UnsafeNumericCast<idx_t>(__builtin_ctzll(positions));
				result[count++] = pending_word_base + bit;
				positions &= positions - 1;
			}
		}

		template <bool ANALYZE_RUNS>
		void ConsumePending(uint8_t next_word_first_bit) {
			const auto valid_mask = MaskForValidBits(pending_valid_bits);
			const auto word = pending_word & valid_mask;
			metrics.active_buckets += UnsafeNumericCast<idx_t>(__builtin_popcountll(word));
			if constexpr (!ANALYZE_RUNS) {
				return;
			}

			const auto previous_bits = (word << 1) | previous_word_last_bit;
			auto next_bits = word >> 1;
			if (pending_valid_bits == 64 && next_word_first_bit) {
				next_bits |= 1ULL << WORD_MASK;
			}

			const auto starts = word & ~previous_bits & valid_mask;
			const auto ends = word & ~next_bits & valid_mask;
			metrics.run_count += UnsafeNumericCast<idx_t>(__builtin_popcountll(starts));
			RecordPositions(starts, metrics.run_starts, metrics.recorded_starts);
			RecordPositions(ends, metrics.run_ends, metrics.recorded_ends);

			previous_word_last_bit =
			    pending_valid_bits == 0 ? 0 : static_cast<uint8_t>((word >> (pending_valid_bits - 1)) & 1ULL);
		}

		BitmapMetrics metrics;
		uint64_t pending_word = 0;
		idx_t pending_valid_bits = 0;
		idx_t pending_word_base = 0;
		uint8_t previous_word_last_bit = 0;
		bool has_pending = false;
	};

	template <typename T, typename CONVERTER>
	idx_t LookupKeysBitmap(Vector &keys, SelectionVector &result_sel, idx_t count) const {
		idx_t found_count = 0;
		for (const auto &entry : keys.template ValidValues<T>()) {
			const U comparable = CONVERTER::Convert(entry.GetValue());
			const U y = comparable - min;
			const uint8_t in_range = y <= span;
			const U bit_idx = ShiftRight(y, shift);
			const uint32_t word_idx = (bit_idx >> WORD_SHIFT) & (0U - in_range);
			const uint8_t bit = (bitmap[word_idx] >> (bit_idx & WORD_MASK)) & 1ULL;

			result_sel.set_index(found_count, entry.GetIndex());
			found_count += bit & in_range;
		}
		return found_count;
	}

	template <typename T, typename CONVERTER>
	idx_t LookupKeysDirect(Vector &keys, SelectionVector &result_sel, idx_t count) const {
		switch (range_count) {
		case 1:
			return LookupKeysDirect<T, CONVERTER, 1>(keys, result_sel, count);
		case 2:
			return LookupKeysDirect<T, CONVERTER, 2>(keys, result_sel, count);
		case 3:
			return LookupKeysDirect<T, CONVERTER, 3>(keys, result_sel, count);
		case 4:
			return LookupKeysDirect<T, CONVERTER, 4>(keys, result_sel, count);
		default:
			return 0;
		}
	}

	template <typename T, typename CONVERTER, idx_t RANGE_COUNT>
	idx_t LookupKeysDirect(Vector &keys, SelectionVector &result_sel, idx_t count) const {
		idx_t found_count = 0;
		for (const auto &entry : keys.template ValidValues<T>()) {
			const U comparable = CONVERTER::Convert(entry.GetValue());
			const uint8_t bit = DirectRangeLookup<RANGE_COUNT>(comparable);

			result_sel.set_index(found_count, entry.GetIndex());
			found_count += bit;
		}
		return found_count;
	}

	static U ShiftRight(U value, idx_t shift_p) {
		if (shift_p >= UnsafeNumericCast<idx_t>(sizeof(U) * 8)) {
			return 0;
		}
		return value >> shift_p;
	}

	static uint64_t MaskForValidBits(idx_t valid_bits) {
		if (valid_bits >= 64) {
			return ~0ULL;
		}
		if (valid_bits == 0) {
			return 0;
		}
		return (1ULL << valid_bits) - 1ULL;
	}

	static uint64_t PackMergedPairsTo32(uint64_t word) {
		uint64_t merged = (word | (word >> 1)) & 0x5555555555555555ULL;
		merged = (merged | (merged >> 1)) & 0x3333333333333333ULL;
		merged = (merged | (merged >> 2)) & 0x0F0F0F0F0F0F0F0FULL;
		merged = (merged | (merged >> 4)) & 0x00FF00FF00FF00FFULL;
		merged = (merged | (merged >> 8)) & 0x0000FFFF0000FFFFULL;
		merged = (merged | (merged >> 16)) & 0x00000000FFFFFFFFULL;
		return merged;
	}

	static BitmapStorage AllocateBitmapStorage(ClientContext &context, idx_t words) {
		BitmapStorage result;
		result.data = AllocateBitmap(context, words, result.bitmap);
		result.word_capacity = words;
		return result;
	}

	idx_t CountActiveBuckets() const {
		if (mode != Mode::BITMAP || !bitmap) {
			return current_active_buckets;
		}
		idx_t result = 0;
		for (idx_t word_idx = 0; word_idx < word_count; word_idx++) {
			result += UnsafeNumericCast<idx_t>(__builtin_popcountll(bitmap[word_idx]));
		}
		return result;
	}

	idx_t CountRuns() const {
		if (mode != Mode::BITMAP || !bitmap) {
			return current_run_count;
		}

		idx_t result = 0;
		uint8_t previous_word_last_bit = 0;
		for (idx_t word_idx = 0; word_idx < word_count; word_idx++) {
			const auto word_base = word_idx << WORD_SHIFT;
			const auto valid_bits = MinValue<idx_t>(64, logical_bucket_count - word_base);
			const auto valid_mask = MaskForValidBits(valid_bits);
			const auto word = bitmap[word_idx] & valid_mask;
			const auto previous_bits = (word << 1) | previous_word_last_bit;
			result += UnsafeNumericCast<idx_t>(__builtin_popcountll(word & ~previous_bits & valid_mask));
			previous_word_last_bit = valid_bits == 0 ? 0 : static_cast<uint8_t>((word >> (valid_bits - 1)) & 1ULL);
		}
		return result;
	}

	static uhugeint_t BucketWidth(idx_t shift_p) {
		return uhugeint_t(1) << uhugeint_t(UnsafeNumericCast<uint64_t>(shift_p));
	}

	uhugeint_t CoveredValues(idx_t active_buckets_p, idx_t shift_p) const {
		auto covered_values = Uhugeint::Convert(active_buckets_p) * BucketWidth(shift_p);
		const auto domain_size = Uhugeint::Convert(span) + 1;
		if (Uhugeint::GreaterThan(covered_values, domain_size)) {
			covered_values = domain_size;
		}
		return covered_values;
	}

	double FalsePositiveRate(idx_t active_buckets_p, idx_t shift_p, idx_t positive_lower_bound_p) const {
		return ConservativeFalsePositiveRate(CoveredValues(active_buckets_p, shift_p), positive_lower_bound_p);
	}

	double ConservativeFalsePositiveRate(uhugeint_t covered_values, idx_t positive_lower_bound_p) const {
		const auto domain_size = Uhugeint::Convert(span) + 1;
		if (Uhugeint::GreaterThan(covered_values, domain_size)) {
			covered_values = domain_size;
		}
		const auto positive_lower_bound = Uhugeint::Convert(positive_lower_bound_p);
		if (Uhugeint::LessThanEquals(domain_size, positive_lower_bound) ||
		    Uhugeint::LessThanEquals(covered_values, positive_lower_bound)) {
			return 0;
		}
		const auto false_positives = covered_values - positive_lower_bound;
		const auto negative_values = domain_size - positive_lower_bound;
		return Uhugeint::Cast<double>(false_positives) / Uhugeint::Cast<double>(negative_values);
	}

	U BucketLowerBound(idx_t bucket_idx) const {
		if (shift >= UnsafeNumericCast<idx_t>(sizeof(U) * 8)) {
			return min;
		}
		return min + (UnsafeNumericCast<U>(bucket_idx) << shift);
	}

	U BucketUpperBound(idx_t bucket_idx) const {
		if (bucket_idx + 1 >= logical_bucket_count) {
			return min + span;
		}
		if (shift >= UnsafeNumericCast<idx_t>(sizeof(U) * 8)) {
			return min + span;
		}
		return min + ((UnsafeNumericCast<U>(bucket_idx + 1) << shift) - 1);
	}

	template <idx_t RANGE_COUNT>
	uint8_t DirectRangeLookup(U value) const {
		if constexpr (RANGE_COUNT == 1) {
			return ValueInDirectRange(value, ranges[0]);
		} else if constexpr (RANGE_COUNT == 2) {
			return ValueInDirectRange(value, ranges[0]) | ValueInDirectRange(value, ranges[1]);
		} else if constexpr (RANGE_COUNT == 3) {
			return ValueInDirectRange(value, ranges[0]) | ValueInDirectRange(value, ranges[1]) |
			       ValueInDirectRange(value, ranges[2]);
		} else if constexpr (RANGE_COUNT == 4) {
			return ValueInDirectRange(value, ranges[0]) | ValueInDirectRange(value, ranges[1]) |
			       ValueInDirectRange(value, ranges[2]) | ValueInDirectRange(value, ranges[3]);
		} else {
			return 0;
		}
	}

	bool DirectRangeLookup(U value) const {
		for (idx_t range_idx = 0; range_idx < range_count; range_idx++) {
			if (ValueInDirectRange(value, ranges[range_idx])) {
				return true;
			}
		}
		return false;
	}

	static uint8_t ValueInDirectRange(U value, const DirectRange &range) {
		return static_cast<uint8_t>((value - range.lower) <= range.width);
	}

	static U DirectRangeUpper(const DirectRange &range) {
		return range.lower + range.width;
	}

	BitmapMetrics AnalyzeBitmap(const uint64_t *source, idx_t source_word_count,
	                            idx_t source_logical_bucket_count) const {
		BitmapMetricsBuilder builder;
		for (idx_t word_idx = 0; word_idx < source_word_count; word_idx++) {
			const auto word_base = word_idx << WORD_SHIFT;
			const auto valid_bits = MinValue<idx_t>(64, source_logical_bucket_count - word_base);
			const auto word = source[word_idx] & MaskForValidBits(valid_bits);
			builder.template PushWord<true>(word, valid_bits, word_base);
		}
		return builder.template Finish<true>();
	}

	template <bool ANALYZE_SOURCE, bool ANALYZE_SOURCE_RUNS, bool ANALYZE_DESTINATION_RUNS>
	void AnalyzeAndReduceBlock(const uint64_t *source, idx_t source_word_count, idx_t source_logical_bucket_count,
	                           uint64_t *destination, idx_t destination_logical_bucket_count,
	                           idx_t destination_word_begin, idx_t destination_word_end,
	                           BitmapMetricsBuilder &source_builder, BitmapMetricsBuilder &destination_builder) const {
		for (idx_t destination_word_idx = destination_word_begin; destination_word_idx < destination_word_end;
		     destination_word_idx++) {
			const auto first_source_word_idx = destination_word_idx * 2;
			const auto first_source_word_base = first_source_word_idx << WORD_SHIFT;
			const auto first_valid_bits = MinValue<idx_t>(64, source_logical_bucket_count - first_source_word_base);
			const auto first_source_word = source[first_source_word_idx] & MaskForValidBits(first_valid_bits);
			if constexpr (ANALYZE_SOURCE) {
				source_builder.template PushWord<ANALYZE_SOURCE_RUNS>(first_source_word, first_valid_bits,
				                                                      first_source_word_base);
			}

			const auto low = PackMergedPairsTo32(first_source_word);
			uint64_t high = 0;
			const auto second_source_word_idx = first_source_word_idx + 1;
			if (second_source_word_idx < source_word_count) {
				const auto second_source_word_base = second_source_word_idx << WORD_SHIFT;
				const auto second_valid_bits =
				    MinValue<idx_t>(64, source_logical_bucket_count - second_source_word_base);
				const auto second_source_word = source[second_source_word_idx] & MaskForValidBits(second_valid_bits);
				if constexpr (ANALYZE_SOURCE) {
					source_builder.template PushWord<ANALYZE_SOURCE_RUNS>(second_source_word, second_valid_bits,
					                                                      second_source_word_base);
				}
				high = PackMergedPairsTo32(second_source_word) << 32;
			}

			const auto destination_word_base = destination_word_idx << WORD_SHIFT;
			const auto destination_valid_bits =
			    MinValue<idx_t>(64, destination_logical_bucket_count - destination_word_base);
			const auto packed = (low | high) & MaskForValidBits(destination_valid_bits);
			destination[destination_word_idx] = packed;
			destination_builder.template PushWord<ANALYZE_DESTINATION_RUNS>(packed, destination_valid_bits,
			                                                                destination_word_base);
		}
	}

	template <bool ANALYZE_SOURCE>
	DyadicPassResult AnalyzeAndReduceBitmap(const uint64_t *source, idx_t source_word_count,
	                                        idx_t source_logical_bucket_count, uint64_t *destination,
	                                        idx_t destination_word_count) const {
		const auto destination_logical_bucket_count = (source_logical_bucket_count + 1) >> 1;
		D_ASSERT(destination_word_count == ((destination_logical_bucket_count + 63) >> WORD_SHIFT));

		BitmapMetricsBuilder source_builder;
		BitmapMetricsBuilder destination_builder;
		bool analyze_source_runs = ANALYZE_SOURCE;
		bool analyze_destination_runs = true;
		for (idx_t destination_word_begin = 0; destination_word_begin < destination_word_count;
		     destination_word_begin += METRICS_BLOCK_WORDS) {
			const auto destination_word_end =
			    MinValue<idx_t>(destination_word_begin + METRICS_BLOCK_WORDS, destination_word_count);
			if constexpr (ANALYZE_SOURCE) {
				if (analyze_source_runs) {
					if (analyze_destination_runs) {
						AnalyzeAndReduceBlock<true, true, true>(source, source_word_count, source_logical_bucket_count,
						                                        destination, destination_logical_bucket_count,
						                                        destination_word_begin, destination_word_end,
						                                        source_builder, destination_builder);
					} else {
						AnalyzeAndReduceBlock<true, true, false>(source, source_word_count, source_logical_bucket_count,
						                                         destination, destination_logical_bucket_count,
						                                         destination_word_begin, destination_word_end,
						                                         source_builder, destination_builder);
					}
				} else if (analyze_destination_runs) {
					AnalyzeAndReduceBlock<true, false, true>(source, source_word_count, source_logical_bucket_count,
					                                         destination, destination_logical_bucket_count,
					                                         destination_word_begin, destination_word_end,
					                                         source_builder, destination_builder);
				} else {
					AnalyzeAndReduceBlock<true, false, false>(source, source_word_count, source_logical_bucket_count,
					                                          destination, destination_logical_bucket_count,
					                                          destination_word_begin, destination_word_end,
					                                          source_builder, destination_builder);
				}
				analyze_source_runs &= source_builder.RunCount() <= MAX_DIRECT_RANGES;
			} else if (analyze_destination_runs) {
				AnalyzeAndReduceBlock<false, false, true>(source, source_word_count, source_logical_bucket_count,
				                                          destination, destination_logical_bucket_count,
				                                          destination_word_begin, destination_word_end, source_builder,
				                                          destination_builder);
			} else {
				AnalyzeAndReduceBlock<false, false, false>(source, source_word_count, source_logical_bucket_count,
				                                           destination, destination_logical_bucket_count,
				                                           destination_word_begin, destination_word_end, source_builder,
				                                           destination_builder);
			}
			analyze_destination_runs &= destination_builder.RunCount() <= MAX_DIRECT_RANGES;
		}

		DyadicPassResult result;
		if constexpr (ANALYZE_SOURCE) {
			result.source =
			    analyze_source_runs ? source_builder.template Finish<true>() : source_builder.template Finish<false>();
		}
		result.destination = analyze_destination_runs ? destination_builder.template Finish<true>()
		                                              : destination_builder.template Finish<false>();
		return result;
	}

	bool ShouldAcceptDyadicLevel(idx_t current_words, const BitmapMetrics &candidate, idx_t candidate_shift,
	                             double candidate_false_positive_rate, uhugeint_t current_covered_values,
	                             double max_false_positive_rate) const {
		if (candidate_false_positive_rate > max_false_positive_rate) {
			return false;
		}
		if (candidate.HasExactRanges()) {
			return true;
		}
		if (Uhugeint::LessThanEquals(CoveredValues(candidate.active_buckets, candidate_shift),
		                             current_covered_values)) {
			return true;
		}
		return current_words * sizeof(uint64_t) > BITMAP_CACHE_TARGET_BYTES;
	}

	void SetCachedAnalysis(const BitmapMetrics &metrics, double false_positive_rate) {
		current_active_buckets = metrics.active_buckets;
		current_run_count = metrics.run_count;
		current_run_count_is_exact = metrics.run_count_is_exact;
		cached_false_positive_rate = false_positive_rate;
		analysis_cached = true;
	}

	void SetDirectRanges(const BitmapMetrics &metrics, BitmapStorage &storage) {
		D_ASSERT(metrics.HasExactRanges());
		range_count = metrics.run_count;
		for (idx_t range_idx = 0; range_idx < range_count; range_idx++) {
			const auto lower = BucketLowerBound(metrics.run_starts[range_idx]);
			const auto upper = BucketUpperBound(metrics.run_ends[range_idx]);
			ranges[range_idx] = {lower, static_cast<U>(upper - lower)};
		}
		mode = Mode::DIRECT_RANGES;
		storage.data.Reset();
		storage.bitmap = nullptr;
		storage.word_capacity = 0;
		buf_.Reset();
		bitmap = nullptr;
		word_count = 0;
	}

	void SetBitmapStorage(ClientContext &context, BitmapStorage &current, BitmapStorage &scratch) {
		scratch.data.Reset();
		if (current.word_capacity == word_count) {
			bitmap = current.bitmap;
			buf_ = std::move(current.data);
			return;
		}

		auto final_storage = AllocateBitmapStorage(context, word_count);
		std::copy_n(current.bitmap, word_count, final_storage.bitmap);
		current.data.Reset();
		bitmap = final_storage.bitmap;
		buf_ = std::move(final_storage.data);
	}

	void CompressBitmap(ClientContext &context, double max_false_positive_rate) {
		BitmapStorage current;
		current.data = std::move(buf_);
		current.bitmap = bitmap;
		current.word_capacity = word_count;
		bitmap = nullptr;

		auto current_words = word_count;
		auto current_logical_buckets = logical_bucket_count;
		auto current_shift = shift;
		BitmapMetrics current_metrics;

		BitmapStorage scratch;
		if (current_logical_buckets > 1) {
			auto next_logical_buckets = (current_logical_buckets + 1) >> 1;
			auto next_words = (next_logical_buckets + 63) >> WORD_SHIFT;
			scratch = AllocateBitmapStorage(context, next_words);
			const auto first_pass = AnalyzeAndReduceBitmap<true>(current.bitmap, current_words, current_logical_buckets,
			                                                     scratch.bitmap, next_words);
			current_metrics = first_pass.source;

			base_active_buckets = current_metrics.active_buckets;
			const auto initial_false_positive_rate =
			    FalsePositiveRate(current_metrics.active_buckets, current_shift, base_active_buckets);
			SetCachedAnalysis(current_metrics, initial_false_positive_rate);
			if (initial_false_positive_rate > max_false_positive_rate || current_metrics.active_buckets == 0) {
				compression_finalized = true;
				SetBitmapStorage(context, current, scratch);
				return;
			}

			if (current_metrics.HasExactRanges()) {
				SetDirectRanges(current_metrics, current);
				compression_finalized = true;
				return;
			}

			auto candidate_metrics = first_pass.destination;
			while (true) {
				const auto candidate_shift = current_shift + 1;
				const auto candidate_false_positive_rate =
				    FalsePositiveRate(candidate_metrics.active_buckets, candidate_shift, base_active_buckets);
				const auto current_covered_values = CoveredValues(current_metrics.active_buckets, current_shift);
				if (!ShouldAcceptDyadicLevel(current_words, candidate_metrics, candidate_shift,
				                             candidate_false_positive_rate, current_covered_values,
				                             max_false_positive_rate)) {
					break;
				}

				std::swap(current, scratch);
				current_words = next_words;
				current_logical_buckets = next_logical_buckets;
				current_shift = candidate_shift;
				current_metrics = candidate_metrics;
				word_count = current_words;
				logical_bucket_count = current_logical_buckets;
				shift = current_shift;
				SetCachedAnalysis(current_metrics, candidate_false_positive_rate);

				if (current_metrics.HasExactRanges()) {
					SetDirectRanges(current_metrics, current);
					compression_finalized = true;
					return;
				}
				if (current_logical_buckets <= 1) {
					break;
				}

				next_logical_buckets = (current_logical_buckets + 1) >> 1;
				next_words = (next_logical_buckets + 63) >> WORD_SHIFT;
				D_ASSERT(scratch.word_capacity >= next_words);
				const auto next_pass = AnalyzeAndReduceBitmap<false>(
				    current.bitmap, current_words, current_logical_buckets, scratch.bitmap, next_words);
				candidate_metrics = next_pass.destination;
			}
		} else {
			current_metrics = AnalyzeBitmap(current.bitmap, current_words, current_logical_buckets);
			base_active_buckets = current_metrics.active_buckets;
			const auto initial_false_positive_rate =
			    FalsePositiveRate(current_metrics.active_buckets, current_shift, base_active_buckets);
			SetCachedAnalysis(current_metrics, initial_false_positive_rate);
			if (initial_false_positive_rate <= max_false_positive_rate && current_metrics.HasExactRanges()) {
				SetDirectRanges(current_metrics, current);
				compression_finalized = true;
				return;
			}
		}

		word_count = current_words;
		logical_bucket_count = current_logical_buckets;
		shift = current_shift;
		mode = Mode::BITMAP;
		range_count = 0;
		SetBitmapStorage(context, current, scratch);
		compression_finalized = true;
	}

	bool initialized = false;
	Mode mode = Mode::BITMAP;
	U min;
	U span;
	idx_t shift;
	idx_t logical_bucket_count;
	idx_t word_count;
	idx_t base_active_buckets = 0;
	idx_t current_active_buckets = 0;
	idx_t current_run_count = 0;
	bool current_run_count_is_exact = false;
	double cached_false_positive_rate = 0;
	bool analysis_cached = false;
	bool compression_finalized = false;
	idx_t range_count = 0;
	array<DirectRange, MAX_DIRECT_RANGES> ranges;
	AllocatedData buf_;
	uint64_t *bitmap;
};

template <typename T>
struct NumericConverter {
	using comparable_type = typename MakeUnsigned<T>::type;

	static inline comparable_type Convert(T value) {
		// Overflow is explicitly allowed for unsigned to signed cast
		return static_cast<comparable_type>(value);
	}
};

struct StringPrefixConverter {
	static inline uint32_t Convert(const string_t &value) {
		return value.GetPrefixIntegerComparable();
	}
};

uint32_t StringPrefixComparable(const string_t &value, char padding) {
	array<char, string_t::PREFIX_BYTES> padded_prefix;
	padded_prefix.fill(padding);
	memcpy(padded_prefix.data(), value.GetData(), MinValue<idx_t>(value.GetSize(), string_t::PREFIX_BYTES));
	return string_t(padded_prefix.data(), string_t::PREFIX_BYTES).GetPrefixIntegerComparable();
}

uint32_t StringMinComparable(const Value &value) {
	return StringPrefixConverter::Convert(value.GetValueUnsafe<string_t>());
}

uint32_t StringMaxComparable(const Value &value) {
	const auto max_string = value.GetValueUnsafe<string_t>();
	if (max_string.GetSize() >= string_t::PREFIX_BYTES) {
		return max_string.GetPrefixIntegerComparable();
	}

	// Pad string prefix with 0xFF to keep correctness if max is truncated at \0 char, e.g., ab\0c -> ab
	return StringPrefixComparable(max_string, char(0xFF));
}

template <typename T>
class NumericPrefixRangeFilter : public PrefixRangeFilter {
private:
	using Comparable = typename MakeUnsigned<T>::type;

public:
	void Initialize(ClientContext &context, idx_t number_of_rows, Value min_val, Value max_val,
	                const PrefixRangeFilter::Sizing &sizing) override {
		D_ASSERT(min_val <= max_val);
		D_ASSERT(number_of_rows > 0);
		const auto min = NumericConverter<T>::Convert(min_val.GetValueUnsafe<T>());
		const auto max = NumericConverter<T>::Convert(max_val.GetValueUnsafe<T>());
		bitmap.Initialize(context, min, max - min, sizing.shift);
	}

	unique_ptr<BuildState> InitializeBuildState(ClientContext &context) const override {
		return bitmap.InitializeBuildState(context);
	}

	void InsertKeys(Vector &keys, idx_t count, BuildState &state) const override {
		auto &bitmap_state = state.Cast<PrefixRangeBitmapBuildState>();
		bitmap.template InsertKeys<T, NumericConverter<T>>(keys, count, bitmap_state.bitmap);
	}

	void MergeBuildState(BuildState &state) override {
		bitmap.MergeBuildState(state.Cast<PrefixRangeBitmapBuildState>());
	}

	idx_t LookupKeys(Vector &keys, SelectionVector &result_sel, idx_t count) const override {
		if (keys.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			return bitmap.template LookupOne<T, NumericConverter<T>>(keys.GetValue(0)) ? count : 0;
		}
		return bitmap.template LookupKeys<T, NumericConverter<T>>(keys, result_sel, count);
	}

	FilterPropagateResult LookupRange(const Value &lower_bound, const Value &upper_bound) const override {
		const auto lb = lower_bound.GetValueUnsafe<T>();
		const auto ub = upper_bound.GetValueUnsafe<T>();

		const auto bitmap_min = static_cast<T>(bitmap.Min());
		const auto bitmap_max = static_cast<T>(bitmap.Min() + bitmap.Span());
		if (ub < bitmap_min || lb > bitmap_max) {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}

		const auto adjusted_lb = NumericConverter<T>::Convert(MaxValue<T>(lb, bitmap_min));
		const auto adjusted_ub = NumericConverter<T>::Convert(MinValue<T>(ub, bitmap_max));
		auto result = bitmap.LookupRange(adjusted_lb, adjusted_ub);
		if (result == FilterPropagateResult::FILTER_ALWAYS_TRUE && (lb < bitmap_min || ub > bitmap_max)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return result;
	}

	FilterPropagateResult LookupStatistics(const BaseStatistics &stats) const override {
		if (stats.GetStatsType() != StatisticsType::NUMERIC_STATS || !NumericStats::HasMinMax(stats)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		const auto min = NumericStats::Min(stats);
		const auto max = NumericStats::Max(stats);
		if (min > max) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return LookupRange(min, max);
	}

	bool IsInitialized() const override {
		return bitmap.IsInitialized();
	}

	Analysis Analyze() const override {
		return bitmap.Analyze();
	}

	Analysis Compress(ClientContext &context, double max_false_positive_rate) override {
		return bitmap.Compress(context, max_false_positive_rate);
	}

	CompressionInfo GetCompressionInfo() const override {
		return bitmap.GetCompressionInfo();
	}

private:
	PrefixRangeBitmap<Comparable> bitmap;
};

class StringPrefixRangeFilter : public PrefixRangeFilter {
public:
	void Initialize(ClientContext &context, idx_t number_of_rows, Value min_val, Value max_val,
	                const PrefixRangeFilter::Sizing &sizing) override {
		D_ASSERT(min_val <= max_val);
		D_ASSERT(number_of_rows > 0);
		const auto min = StringPrefixConverter::Convert(min_val.GetValueUnsafe<string_t>());
		const auto max = StringPrefixConverter::Convert(max_val.GetValueUnsafe<string_t>());
		D_ASSERT(min <= max);
		bitmap.Initialize(context, min, max - min, sizing.shift);
	}

	unique_ptr<BuildState> InitializeBuildState(ClientContext &context) const override {
		return bitmap.InitializeBuildState(context);
	}

	void InsertKeys(Vector &keys, idx_t count, BuildState &state) const override {
		auto &bitmap_state = state.Cast<PrefixRangeBitmapBuildState>();
		bitmap.template InsertKeys<string_t, StringPrefixConverter>(keys, count, bitmap_state.bitmap);
	}

	void MergeBuildState(BuildState &state) override {
		bitmap.MergeBuildState(state.Cast<PrefixRangeBitmapBuildState>());
	}

	idx_t LookupKeys(Vector &keys, SelectionVector &result_sel, idx_t count) const override {
		if (keys.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			return bitmap.template LookupOne<string_t, StringPrefixConverter>(keys.GetValue(0)) ? count : 0;
		}
		return bitmap.template LookupKeys<string_t, StringPrefixConverter>(keys, result_sel, count);
	}

	FilterPropagateResult LookupRange(const Value &lower_bound, const Value &upper_bound) const override {
		auto lower_bound_comparable = StringMinComparable(lower_bound);
		auto upper_bound_comparable = StringMaxComparable(upper_bound);
		if (lower_bound_comparable > upper_bound_comparable) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}

		const auto bitmap_min = bitmap.Min();
		const auto bitmap_max = bitmap.Min() + bitmap.Span();
		if (upper_bound_comparable < bitmap_min || lower_bound_comparable > bitmap_max) {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}

		lower_bound_comparable = MaxValue<uint32_t>(lower_bound_comparable, bitmap_min);
		upper_bound_comparable = MinValue<uint32_t>(upper_bound_comparable, bitmap_max);
		auto result = bitmap.LookupRange(lower_bound_comparable, upper_bound_comparable);
		if (result == FilterPropagateResult::FILTER_ALWAYS_TRUE &&
		    (StringMinComparable(lower_bound) < bitmap_min || StringMaxComparable(upper_bound) > bitmap_max)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return result;
	}

	FilterPropagateResult LookupStatistics(const BaseStatistics &stats) const override {
		if (stats.GetStatsType() != StatisticsType::STRING_STATS || !StringStats::HasMinMax(stats)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}

		const auto min_string = StringStats::Min(stats);
		const auto max_string = StringStats::Max(stats);
		const auto min =
		    StringPrefixComparable(string_t(min_string.data(), UnsafeNumericCast<uint32_t>(min_string.size())), '\0');
		const auto max_padding = StringStats::GetMaxType(stats) == StringStatsType::TRUNCATED_STATS ? char(0xFF) : '\0';
		const auto max = StringPrefixComparable(
		    string_t(max_string.data(), UnsafeNumericCast<uint32_t>(max_string.size())), max_padding);
		if (min > max) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}

		const auto bitmap_min = bitmap.Min();
		const auto bitmap_max = bitmap.Min() + bitmap.Span();
		if (max < bitmap_min || min > bitmap_max) {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		return bitmap.LookupRange(MaxValue<uint32_t>(min, bitmap_min), MinValue<uint32_t>(max, bitmap_max));
	}

	bool IsInitialized() const override {
		return bitmap.IsInitialized();
	}

	Analysis Analyze() const override {
		return bitmap.Analyze();
	}

	Analysis Compress(ClientContext &context, double max_false_positive_rate) override {
		return bitmap.Compress(context, max_false_positive_rate);
	}

	CompressionInfo GetCompressionInfo() const override {
		return bitmap.GetCompressionInfo();
	}

private:
	PrefixRangeBitmap<uint32_t> bitmap;
};

template <typename T>
bool ComputeSpan(const Value &lower_bound, const Value &upper_bound, uhugeint_t &result) {
	T lb_value = lower_bound.GetValueUnsafe<T>();
	T ub_value = upper_bound.GetValueUnsafe<T>();
	T res;
	if (TrySubtractOperator::Operation(ub_value, lb_value, res)) {
		result = Uhugeint::Convert(res);
		return true;
	} else {
		return false;
	}
}

bool ComputeStringPrefixSpan(const Value &lower_bound, const Value &upper_bound, uhugeint_t &result) {
#ifdef DUCKDB_DEBUG_NO_INLINE
	return false;
#else
	auto lb_value = lower_bound.GetValueUnsafe<string_t>().GetPrefixIntegerComparable();
	auto ub_value = upper_bound.GetValueUnsafe<string_t>().GetPrefixIntegerComparable();
	uint32_t res;
	if (TrySubtractOperator::Operation(ub_value, lb_value, res)) {
		result = Uhugeint::Convert(res);
		return true;
	} else {
		return false;
	}
#endif
}

bool PrefixRangeFilter::TryComputeBucketCount(const uhugeint_t &span, idx_t shift, idx_t &bucket_count) {
	if (shift < 0) {
		return false;
	}
	const auto shifted_span = span >> uhugeint_t(UnsafeNumericCast<uint64_t>(shift));
	auto bucket_count_huge = shifted_span;
	if (!Uhugeint::TryAddInPlace(bucket_count_huge, 1)) {
		return false;
	}
	return Uhugeint::TryCast(bucket_count_huge, bucket_count);
}

double PrefixRangeFilter::ComputeFalsePositiveRateUpperBound(const uhugeint_t &span, idx_t count, idx_t shift) {
	if (count == 0 || Uhugeint::LessThanEquals(span, Uhugeint::Convert(count))) {
		return 0;
	}
	D_ASSERT(shift >= 0);
	const auto bucket_width = uhugeint_t(1) << uhugeint_t(UnsafeNumericCast<uint64_t>(shift));
	const auto numerator = Uhugeint::Cast<double>(Uhugeint::Convert(count) * (bucket_width - 1));
	const auto denominator = Uhugeint::Cast<double>(span - Uhugeint::Convert(count));
	D_ASSERT(denominator > 0);
	return numerator / denominator;
}

double PrefixRangeFilter::EstimateFalsePositiveRate(const uhugeint_t &span, idx_t key_count, idx_t active_buckets,
                                                    idx_t shift) {
	if (key_count == 0) {
		return 0;
	}
	const auto total_values = span + 1;
	if (Uhugeint::LessThanEquals(total_values, Uhugeint::Convert(key_count))) {
		return 0;
	}
	D_ASSERT(shift >= 0);
	const auto bucket_width = uhugeint_t(1) << uhugeint_t(UnsafeNumericCast<uint64_t>(shift));
	auto covered_values = Uhugeint::Convert(active_buckets) * bucket_width;
	if (Uhugeint::GreaterThan(covered_values, total_values)) {
		covered_values = total_values;
	}
	if (Uhugeint::LessThanEquals(covered_values, Uhugeint::Convert(key_count))) {
		return 0;
	}
	const auto false_positives = covered_values - Uhugeint::Convert(key_count);
	const auto negative_values = total_values - Uhugeint::Convert(key_count);
	return Uhugeint::Cast<double>(false_positives) / Uhugeint::Cast<double>(negative_values);
}

unique_ptr<PrefixRangeFilter> PrefixRangeFilter::CreatePrefixRangeFilter(const LogicalType &key_type) {
	switch (key_type.InternalType()) {
	case PhysicalType::UINT8:
		return make_uniq<NumericPrefixRangeFilter<uint8_t>>();
	case PhysicalType::UINT16:
		return make_uniq<NumericPrefixRangeFilter<uint16_t>>();
	case PhysicalType::UINT32:
		return make_uniq<NumericPrefixRangeFilter<uint32_t>>();
	case PhysicalType::UINT64:
		return make_uniq<NumericPrefixRangeFilter<uint64_t>>();
	case PhysicalType::INT8:
		return make_uniq<NumericPrefixRangeFilter<int8_t>>();
	case PhysicalType::INT16:
		return make_uniq<NumericPrefixRangeFilter<int16_t>>();
	case PhysicalType::INT32:
		return make_uniq<NumericPrefixRangeFilter<int32_t>>();
	case PhysicalType::INT64:
		return make_uniq<NumericPrefixRangeFilter<int64_t>>();
	case PhysicalType::VARCHAR:
#ifdef DUCKDB_DEBUG_NO_INLINE
		throw NotImplementedException("Prefix range filter is not implemented for type %s", key_type.ToString());
#else
		return make_uniq<StringPrefixRangeFilter>();
#endif
	case PhysicalType::INT128:
	case PhysicalType::UINT128:
	default:
		throw NotImplementedException("Prefix range filter is not implemented for type %s", key_type.ToString());
	}
}

bool PrefixRangeFilter::TryComputeSpan(const Value &lower_bound, const Value &upper_bound, uhugeint_t &result) {
	if (lower_bound.type().InternalType() != upper_bound.type().InternalType()) {
		return false;
	}

	switch (lower_bound.type().InternalType()) {
	case PhysicalType::UINT8:
		return ComputeSpan<uint8_t>(lower_bound, upper_bound, result);
	case PhysicalType::UINT16:
		return ComputeSpan<uint16_t>(lower_bound, upper_bound, result);
	case PhysicalType::UINT32:
		return ComputeSpan<uint32_t>(lower_bound, upper_bound, result);
	case PhysicalType::UINT64:
		return ComputeSpan<uint64_t>(lower_bound, upper_bound, result);
	case PhysicalType::INT8:
		return ComputeSpan<int8_t>(lower_bound, upper_bound, result);
	case PhysicalType::INT16:
		return ComputeSpan<int16_t>(lower_bound, upper_bound, result);
	case PhysicalType::INT32:
		return ComputeSpan<int32_t>(lower_bound, upper_bound, result);
	case PhysicalType::INT64:
		return ComputeSpan<int64_t>(lower_bound, upper_bound, result);
	case PhysicalType::VARCHAR:
		return ComputeStringPrefixSpan(lower_bound, upper_bound, result);
	case PhysicalType::INT128:
	case PhysicalType::UINT128:
	default:
		return false;
	}
}

bool PrefixRangeFilter::TryComputeSizing(const Value &min, const Value &max, idx_t count, Sizing &sizing,
                                         double false_positive_rate) {
	if (count == 0 || false_positive_rate < 0) {
		return false;
	}
	if (!TryComputeSpan(min, max, sizing.span)) {
		return false;
	}
	if (sizing.span == 0) {
		return false;
	}
	if (Uhugeint::LessThanEquals(sizing.span, Uhugeint::Convert(count))) {
		sizing.shift = 0;
		return true;
	}

	idx_t best_shift = 0;
	for (idx_t shift = 1; shift < 64; shift++) {
		if (ComputeFalsePositiveRateUpperBound(sizing.span, count, shift) > false_positive_rate) {
			break;
		}
		best_shift = shift;
	}
	sizing.shift = best_shift;
	return true;
}

bool PrefixRangeFilter::TryComputeFixedSizeSizing(const Value &min, const Value &max, idx_t bucket_count_limit,
                                                  Sizing &sizing) {
	if (bucket_count_limit == 0) {
		return false;
	}
	if (!TryComputeSpan(min, max, sizing.span)) {
		return false;
	}
	if (sizing.span == 0) {
		return false;
	}
	sizing.shift = 0;
	idx_t bucket_count;
	while (TryComputeBucketCount(sizing.span, sizing.shift, bucket_count) && bucket_count > bucket_count_limit) {
		sizing.shift++;
	}
	return TryComputeBucketCount(sizing.span, sizing.shift, bucket_count) && bucket_count <= bucket_count_limit;
}

bool PrefixRangeFilter::SupportedType(const LogicalType &type) {
	switch (type.InternalType()) {
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
		return true;
	case PhysicalType::VARCHAR:
#ifdef DUCKDB_DEBUG_NO_INLINE
		return false;
#else
		return true;
#endif
	case PhysicalType::INT128:
	case PhysicalType::UINT128:
	default:
		return false;
	}
}

PrefixRangeFunctionData::PrefixRangeFunctionData(optional_ptr<PrefixRangeFilter> filter_p,
                                                 const string &key_column_name_p, const LogicalType &key_type_p,
                                                 float selectivity_threshold_p, idx_t n_vectors_to_check_p)
    : filter(filter_p), key_column_name(key_column_name_p), key_type(key_type_p),
      selectivity_threshold(selectivity_threshold_p), n_vectors_to_check(n_vectors_to_check_p) {
}

unique_ptr<FunctionData> PrefixRangeFunctionData::Copy() const {
	return make_uniq<PrefixRangeFunctionData>(filter, key_column_name, key_type, selectivity_threshold,
	                                          n_vectors_to_check);
}

bool PrefixRangeFunctionData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<PrefixRangeFunctionData>();
	return filter.get() == other.filter.get() && key_column_name == other.key_column_name && key_type == other.key_type;
}

static idx_t SelectPrefixRange(Vector &input, const PrefixRangeFunctionData &func_data, SelectionVector &result_sel,
                               idx_t count) {
	D_ASSERT(func_data.filter);
	return func_data.filter->LookupKeys(input, result_sel, count);
}

static unique_ptr<FunctionLocalState>
PrefixRangeInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr, FunctionData *bind_data) {
	auto &data = bind_data->Cast<PrefixRangeFunctionData>();
	if (!data.filter) {
		return nullptr;
	}
	return InitSelectivityTrackingLocalState(data.n_vectors_to_check, data.selectivity_threshold);
}

static idx_t PrefixRangeSelect(DataChunk &args, ExpressionState &state, optional_ptr<const SelectionVector> sel,
                               optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &func_data = func_expr.bind_info->Cast<PrefixRangeFunctionData>();
	auto local_state_ptr = ExecuteFunctionState::GetFunctionState(state);
	auto tracking_state = local_state_ptr ? &local_state_ptr->Cast<SelectivityTrackingLocalState>() : nullptr;

	auto count = args.size();
	if (!func_data.filter || !func_data.filter->IsInitialized()) {
		return SetAllTrueSelection(count, sel, true_sel, false_sel);
	}
	if (!func_data.filter->AllowsTupleFiltering()) {
		return SetAllTrueSelection(count, sel, true_sel, false_sel);
	}
	if (tracking_state && !tracking_state->IsActive()) {
		tracking_state->Update(0, 0);
		return SetAllTrueSelection(count, sel, true_sel, false_sel);
	}

	SelectionVector temp_true(count);
	auto result_true_sel = (!true_sel || (sel && true_sel.get() == sel.get())) ? &temp_true : true_sel.get();
	auto approved_count = SelectPrefixRange(args.data[0], func_data, *result_true_sel, count);
	approved_count = TranslateSelection(count, sel, *result_true_sel, approved_count, true_sel, false_sel);
	if (tracking_state) {
		tracking_state->Update(approved_count, count);
	}
	return approved_count;
}

ScalarFunction PrefixRangeScalarFun::GetFunction(const LogicalType &input_type) {
	ScalarFunction func(NAME, {input_type}, LogicalType::BOOLEAN, nullptr, TableFilterFunctions::Bind);
	func.SetInitStateCallback(PrefixRangeInitLocalState);
	func.SetSelectCallback(PrefixRangeSelect);
	func.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	func.SetFilterPruneCallback(PrefixRangeScalarFun::FilterPrune);
	func.SetSerializeCallback(TableFilterFunctionSerialize);
	func.SetDeserializeCallback(TableFilterFunctionDeserialize);
	return func;
}

string PrefixRangeScalarFun::ToString(const string &column_name, const string &key_column_name) {
	return column_name + " IN PRF(" + key_column_name + ")";
}

FilterPropagateResult PrefixRangeScalarFun::FilterPrune(const FunctionStatisticsPruneInput &input) {
	if (!input.bind_data) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &data = input.bind_data->Cast<PrefixRangeFunctionData>();
	if (!data.filter || !data.filter->IsInitialized()) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	return data.filter->LookupStatistics(input.stats);
}

ScalarFunction TableFilterPrefixRangeFun::GetFunction() {
	return PrefixRangeScalarFun::GetFunction(LogicalType::ANY);
}

} // namespace duckdb
