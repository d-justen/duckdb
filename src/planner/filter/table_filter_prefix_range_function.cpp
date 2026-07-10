//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/filter/table_filter_prefix_range_function.cpp
//
//
//===----------------------------------------------------------------------===//

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
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/filter/table_filter_function_helpers.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

#include <algorithm>

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
	void InsertKeys(Vector &keys, uint64_t *state_bitmap) const {
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

	template <typename T, typename CONVERTER>
	idx_t LookupKeys(Vector &keys, const SelectionVector &sel, SelectionVector &result_sel, idx_t count) const {
		UnifiedVectorFormat key_data;
		keys.ToUnifiedFormat(key_data);
		const auto keys_data = UnifiedVectorFormat::GetData<T>(key_data);
		idx_t found_count = 0;
		for (idx_t i = 0; i < count; i++) {
			const auto source_idx = sel.get_index_unsafe(i);
			const auto key_idx = key_data.sel->get_index(source_idx);
			if (!key_data.validity.RowIsValid(key_idx)) {
				continue;
			}
			const U comparable = CONVERTER::Convert(keys_data[key_idx]);
			bool matches;
			if (mode == Mode::DIRECT_RANGES) {
				matches = DirectRangeLookup(comparable);
			} else {
				const U y = comparable - min;
				const uint8_t in_range = y <= span;
				const U bit_idx = ShiftRight(y, shift);
				const uint32_t word_idx = (bit_idx >> WORD_SHIFT) & (0U - in_range);
				matches = ((bitmap[word_idx] >> (bit_idx & WORD_MASK)) & 1ULL) && in_range;
			}
			if (matches) {
				result_sel.set_index(found_count++, i);
			}
		}
		return found_count;
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
		if (mode == Mode::DIRECT_RANGES) {
			return {active_buckets, direct_false_positive_rate};
		}

		const auto current_active_buckets = CountActiveBuckets();
		return {current_active_buckets, ConservativeFalsePositiveRate(
		                                    Uhugeint::Convert(current_active_buckets) * BucketWidth(), active_buckets)};
	}

	void Compress(ClientContext &context, double max_false_positive_rate) {
		if (!initialized || mode != Mode::BITMAP || max_false_positive_rate < 0) {
			return;
		}
		active_buckets = CountActiveBuckets();
		if (TryCompressToDirectRanges(max_false_positive_rate)) {
			return;
		}
		TryCompressDyadically(context, max_false_positive_rate);
	}

	PrefixRangeFilter::CompressionInfo GetCompressionInfo() const {
		PrefixRangeFilter::CompressionInfo info;
		info.mode = mode == Mode::DIRECT_RANGES ? PrefixRangeCompressionMode::DIRECT_RANGES
		                                        : PrefixRangeCompressionMode::BITMAP;
		info.shift = shift;
		info.range_count = range_count;
		info.active_buckets = mode == Mode::DIRECT_RANGES ? active_buckets : CountActiveBuckets();
		info.logical_bucket_count = logical_bucket_count;
		info.false_positive_rate =
		    mode == Mode::DIRECT_RANGES
		        ? direct_false_positive_rate
		        : ConservativeFalsePositiveRate(Uhugeint::Convert(info.active_buckets) * BucketWidth(), active_buckets);
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
	static constexpr idx_t MAX_DIRECT_GAPS = MAX_DIRECT_RANGES - 1;

	enum class Mode : uint8_t { BITMAP, DIRECT_RANGES };

	struct DirectRange {
		U lower;
		U width;
	};

	struct Gap {
		idx_t start;
		idx_t length;
	};

	struct BitmapStorage {
		AllocatedData data;
		uint64_t *bitmap = nullptr;
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

	static bool IsGapLarger(const Gap &lhs, const Gap &rhs) {
		if (lhs.length != rhs.length) {
			return lhs.length > rhs.length;
		}
		return lhs.start < rhs.start;
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

	static void MaybeInsertTopGap(array<Gap, MAX_DIRECT_GAPS> &top_gaps, idx_t &top_gap_count, Gap candidate) {
		if (candidate.length == 0) {
			return;
		}
		if (top_gap_count < MAX_DIRECT_GAPS) {
			top_gaps[top_gap_count++] = candidate;
		} else {
			idx_t smallest_idx = 0;
			for (idx_t i = 1; i < top_gap_count; i++) {
				if (IsGapLarger(top_gaps[smallest_idx], top_gaps[i])) {
					smallest_idx = i;
				}
			}
			if (!IsGapLarger(candidate, top_gaps[smallest_idx])) {
				return;
			}
			top_gaps[smallest_idx] = candidate;
		}
		std::sort(top_gaps.begin(), top_gaps.begin() + top_gap_count,
		          [](const Gap &lhs, const Gap &rhs) { return IsGapLarger(lhs, rhs); });
	}

	static idx_t TopGapLengthSum(const array<Gap, MAX_DIRECT_GAPS> &top_gaps, idx_t top_gap_count, idx_t keep_count) {
		const auto limit = MinValue<idx_t>(top_gap_count, keep_count);
		idx_t sum = 0;
		for (idx_t i = 0; i < limit; i++) {
			sum += top_gaps[i].length;
		}
		return sum;
	}

	bool ShouldRejectDirectRangeCompression(const array<Gap, MAX_DIRECT_GAPS> &top_gaps, idx_t top_gap_count,
	                                        idx_t previous_set_bucket, double max_false_positive_rate) const {
		array<Gap, MAX_DIRECT_GAPS> optimistic_gaps = top_gaps;
		auto optimistic_gap_count = top_gap_count;
		if (previous_set_bucket + 1 < logical_bucket_count - 1) {
			MaybeInsertTopGap(optimistic_gaps, optimistic_gap_count,
			                  Gap {previous_set_bucket + 1, logical_bucket_count - previous_set_bucket - 2});
		}
		const auto removed_buckets = TopGapLengthSum(optimistic_gaps, optimistic_gap_count, MAX_DIRECT_GAPS);
		const auto covered_buckets = logical_bucket_count - removed_buckets;
		const auto optimistic_false_positive_rate =
		    ConservativeFalsePositiveRate(Uhugeint::Convert(covered_buckets) * BucketWidth(), active_buckets);
		return optimistic_false_positive_rate > max_false_positive_rate;
	}

	static BitmapStorage AllocateBitmapStorage(ClientContext &context, idx_t words) {
		BitmapStorage result;
		result.data = AllocateBitmap(context, words, result.bitmap);
		return result;
	}

	idx_t CountActiveBuckets() const {
		idx_t result = 0;
		for (idx_t word_idx = 0; word_idx < word_count; word_idx++) {
			result += UnsafeNumericCast<idx_t>(__builtin_popcountll(bitmap[word_idx]));
		}
		return result;
	}

	uhugeint_t BucketWidth() const {
		return uhugeint_t(1) << uhugeint_t(UnsafeNumericCast<uint64_t>(shift));
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
		return UnsafeNumericCast<U>(min + (UnsafeNumericCast<U>(bucket_idx) << shift));
	}

	U BucketUpperBound(idx_t bucket_idx) const {
		if (bucket_idx + 1 >= logical_bucket_count) {
			return min + span;
		}
		if (shift >= UnsafeNumericCast<idx_t>(sizeof(U) * 8)) {
			return min + span;
		}
		return UnsafeNumericCast<U>(min + ((UnsafeNumericCast<U>(bucket_idx + 1) << shift) - 1));
	}

	template <idx_t RANGE_COUNT>
	uint8_t DirectRangeLookup(U value) const {
		if constexpr (RANGE_COUNT == 1) {
			return static_cast<uint8_t>((value - min) <= span);
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

	bool TryCompressToDirectRanges(double max_false_positive_rate) {
		if (logical_bucket_count == 0 || active_buckets == 0 || active_buckets > logical_bucket_count) {
			return false;
		}
		// Signed numeric keys use their raw unsigned representation, so a range spanning zero wraps around the
		// unsigned domain. The bitmap supports that layout, but direct ranges require a linear ordering.
		if (span > NumericLimits<U>::Maximum() - min) {
			return false;
		}

		array<Gap, MAX_DIRECT_GAPS> top_gaps;
		top_gaps.fill({0, 0});
		idx_t top_gap_count = 0;
		bool have_previous_set = false;
		idx_t previous_set_bucket = 0;

		for (idx_t word_idx = 0; word_idx < word_count; word_idx++) {
			const auto word_base = word_idx << WORD_SHIFT;
			if (word_base >= logical_bucket_count) {
				break;
			}
			const auto valid_bits = MinValue<idx_t>(64, logical_bucket_count - word_base);
			const uint64_t word = bitmap[word_idx] & MaskForValidBits(valid_bits);

			uint64_t remaining = word;
			while (remaining != 0) {
				const auto bit = UnsafeNumericCast<idx_t>(__builtin_ctzll(remaining));
				const auto bucket = word_base + bit;
				if (have_previous_set) {
					MaybeInsertTopGap(top_gaps, top_gap_count,
					                  Gap {previous_set_bucket + 1, bucket - previous_set_bucket - 1});
				}
				have_previous_set = true;
				previous_set_bucket = bucket;
				remaining &= remaining - 1;
			}

			if (have_previous_set && ShouldRejectDirectRangeCompression(top_gaps, top_gap_count, previous_set_bucket,
			                                                            max_false_positive_rate)) {
				return false;
			}
		}

		if (!have_previous_set) {
			return false;
		}

		for (idx_t direct_range_count = 1; direct_range_count <= MAX_DIRECT_RANGES; direct_range_count++) {
			const auto kept_gap_count = direct_range_count - 1;
			const auto actual_kept_gap_count = MinValue<idx_t>(kept_gap_count, top_gap_count);
			double false_positive_rate;
			const auto removed_buckets = TopGapLengthSum(top_gaps, top_gap_count, actual_kept_gap_count);
			const auto covered_buckets = logical_bucket_count - removed_buckets;
			false_positive_rate =
			    ConservativeFalsePositiveRate(Uhugeint::Convert(covered_buckets) * BucketWidth(), active_buckets);
			if (false_positive_rate > max_false_positive_rate) {
				continue;
			}

			array<Gap, MAX_DIRECT_GAPS> selected_gaps = top_gaps;
			std::sort(selected_gaps.begin(), selected_gaps.begin() + actual_kept_gap_count,
			          [](const Gap &lhs, const Gap &rhs) { return lhs.start < rhs.start; });

			idx_t next_bucket = 0;
			range_count = 0;
			for (idx_t gap_idx = 0; gap_idx < actual_kept_gap_count; gap_idx++) {
				const auto &gap = selected_gaps[gap_idx];
				if (next_bucket <= gap.start - 1) {
					const auto lower = BucketLowerBound(next_bucket);
					const auto upper = BucketUpperBound(gap.start - 1);
					ranges[range_count++] = {lower, static_cast<U>(upper - lower)};
				}
				next_bucket = gap.start + gap.length;
			}
			if (next_bucket < logical_bucket_count) {
				const auto lower = BucketLowerBound(next_bucket);
				const auto upper = BucketUpperBound(logical_bucket_count - 1);
				ranges[range_count++] = {lower, static_cast<U>(upper - lower)};
			}
			if (range_count == 0 || range_count > MAX_DIRECT_RANGES) {
				return false;
			}
			mode = Mode::DIRECT_RANGES;
			direct_false_positive_rate = false_positive_rate;
			return true;
		}
		return false;
	}

	idx_t ReduceBitmapDyadically(const uint64_t *source, idx_t source_word_count, idx_t source_logical_bucket_count,
	                             uint64_t *destination, idx_t destination_word_count) const {
		const auto next_logical_bucket_count = (source_logical_bucket_count + 1) >> 1;
		D_ASSERT(destination_word_count == ((next_logical_bucket_count + 63) >> WORD_SHIFT));
		idx_t next_active_buckets = 0;
		idx_t dst_word_idx = 0;
		for (idx_t src_word_idx = 0; dst_word_idx < destination_word_count; src_word_idx += 2, dst_word_idx++) {
			const uint64_t low = PackMergedPairsTo32(source[src_word_idx]);
			const uint64_t high =
			    src_word_idx + 1 < source_word_count ? (PackMergedPairsTo32(source[src_word_idx + 1]) << 32) : 0ULL;
			const uint64_t packed = low | high;
			destination[dst_word_idx] = packed;
			next_active_buckets += UnsafeNumericCast<idx_t>(__builtin_popcountll(packed));
		}
		if ((next_logical_bucket_count & WORD_MASK) != 0) {
			destination[destination_word_count - 1] &= MaskForValidBits(next_logical_bucket_count & WORD_MASK);
		}
		return next_active_buckets;
	}

	void TryCompressDyadically(ClientContext &context, double max_false_positive_rate) {
		if (logical_bucket_count <= 1) {
			return;
		}

		auto current_words = word_count;
		auto current_logical_buckets = logical_bucket_count;
		auto current_shift = shift;
		auto current_bitmap = bitmap;

		BitmapStorage scratch = AllocateBitmapStorage(
		    context, MaxValue<idx_t>(1, (((current_logical_buckets + 1) >> 1) + 63) >> WORD_SHIFT));
		BitmapStorage accepted;
		bool changed = false;

		while (current_logical_buckets > 1) {
			const auto next_logical_buckets = (current_logical_buckets + 1) >> 1;
			const auto next_words = (next_logical_buckets + 63) >> WORD_SHIFT;
			if (scratch.data.GetSize() < next_words * sizeof(uint64_t)) {
				scratch = AllocateBitmapStorage(context, next_words);
			}
			const auto next_active_buckets = ReduceBitmapDyadically(
			    current_bitmap, current_words, current_logical_buckets, scratch.bitmap, next_words);
			const auto next_shift = current_shift + 1;
			const auto previous_shift = shift;
			shift = next_shift;
			const auto false_positive_rate =
			    ConservativeFalsePositiveRate(Uhugeint::Convert(next_active_buckets) * BucketWidth(), active_buckets);
			shift = previous_shift;
			if (false_positive_rate > max_false_positive_rate) {
				break;
			}

			accepted = AllocateBitmapStorage(context, next_words);
			std::copy_n(scratch.bitmap, next_words, accepted.bitmap);
			current_bitmap = accepted.bitmap;
			current_words = next_words;
			current_logical_buckets = next_logical_buckets;
			current_shift = next_shift;
			changed = true;
		}

		if (!changed) {
			return;
		}

		buf_ = std::move(accepted.data);
		bitmap = accepted.bitmap;
		word_count = current_words;
		logical_bucket_count = current_logical_buckets;
		shift = current_shift;
	}

	bool initialized = false;
	Mode mode = Mode::BITMAP;
	U min;
	U span;
	idx_t shift;
	idx_t logical_bucket_count;
	idx_t word_count;
	idx_t active_buckets = 0;
	double direct_false_positive_rate = 0;
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

	void InsertKeys(Vector &keys, BuildState &state) const override {
		auto &bitmap_state = state.Cast<PrefixRangeBitmapBuildState>();
		bitmap.template InsertKeys<T, NumericConverter<T>>(keys, bitmap_state.bitmap);
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

	idx_t LookupKeys(Vector &keys, const SelectionVector &sel, SelectionVector &result_sel,
	                 idx_t count) const override {
		return bitmap.template LookupKeys<T, NumericConverter<T>>(keys, sel, result_sel, count);
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

	void Compress(ClientContext &context, double max_false_positive_rate) override {
		bitmap.Compress(context, max_false_positive_rate);
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

	void InsertKeys(Vector &keys, BuildState &state) const override {
		auto &bitmap_state = state.Cast<PrefixRangeBitmapBuildState>();
		bitmap.template InsertKeys<string_t, StringPrefixConverter>(keys, bitmap_state.bitmap);
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

	idx_t LookupKeys(Vector &keys, const SelectionVector &sel, SelectionVector &result_sel,
	                 idx_t count) const override {
		return bitmap.template LookupKeys<string_t, StringPrefixConverter>(keys, sel, result_sel, count);
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

	void Compress(ClientContext &context, double max_false_positive_rate) override {
		bitmap.Compress(context, max_false_positive_rate);
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
	auto &func_data = func_expr.BindInfo()->Cast<PrefixRangeFunctionData>();
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
	auto child_stats = input.ChildStats(0);
	if (!child_stats) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	return data.filter->LookupStatistics(*child_stats);
}

ScalarFunction TableFilterPrefixRangeFun::GetFunction() {
	return PrefixRangeScalarFun::GetFunction(LogicalType::ANY);
}

} // namespace duckdb
