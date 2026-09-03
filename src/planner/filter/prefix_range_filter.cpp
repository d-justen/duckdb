//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/filter/prefix_range_filter.cpp
//
//
//===----------------------------------------------------------------------===//

#include "duckdb/planner/filter/prefix_range_filter.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/array.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/enums/vector_type.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/storage/buffer_manager.hpp"

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
		if (shift >= sizeof(U) * 8) {
			throw InternalException("Invalid prefix range filter shift");
		}

		idx_t buckets;
		if (!PrefixRangeFilter::TryComputeBucketCount(Uhugeint::Convert(span), shift, buckets)) {
			throw InternalException("Invalid prefix range filter sizing");
		}
		word_count = buckets / 64 + (buckets % 64 != 0);

		buf_ = AllocateBitmap(context, word_count, bitmap);

		// Only mark initialized as true when local bitmaps are merged.
		initialized = false;
	}

	unique_ptr<PrefixRangeBitmapBuildState> InitializeBuildState(ClientContext &context) const {
		D_ASSERT(bitmap);
		uint64_t *state_bitmap;
		auto state_data = AllocateBitmap(context, word_count, state_bitmap);
		return make_uniq<PrefixRangeBitmapBuildState>(std::move(state_data), state_bitmap);
	}

	template <bool PARALLEL>
	static void SetBit(uint64_t *state_bitmap, idx_t bit_idx) {
		const auto word_idx = bit_idx >> WORD_SHIFT;
		const auto mask = 1ULL << (bit_idx & WORD_MASK);
		if (PARALLEL) {
			// Shared build lanes are published only after every finalize task completes.
			auto &slot = *reinterpret_cast<atomic<uint64_t> *>(&state_bitmap[word_idx]);
			slot.fetch_or(mask, std::memory_order_relaxed);
		} else {
			state_bitmap[word_idx] |= mask;
		}
	}

	template <typename T, typename CONVERTER, bool PARALLEL>
	void InsertKeys(Vector &keys, uint64_t *state_bitmap) const {
		for (const auto &entry : keys.template ValidValues<T>()) {
			const U y = CONVERTER::Convert(entry.GetValue()) - min;
			// All keys are in-range by construction, so the range check can be omitted here.
			const U idx = y >> shift;
			SetBit<PARALLEL>(state_bitmap, UnsafeNumericCast<idx_t>(idx));
		}
	}

	void MergeBuildState(PrefixRangeBitmapBuildState &state) {
		for (idx_t word_idx = 0; word_idx < word_count; word_idx++) {
			bitmap[word_idx] |= state.bitmap[word_idx];
		}
		initialized = true;
	}

	idx_t GetBuildStateSize() const {
		return word_count * sizeof(uint64_t) + 64;
	}

	template <typename T, typename CONVERTER>
	inline bool LookupOne(const Value &value) const {
		if (value.IsNull()) {
			return false;
		}

		const U comparable = CONVERTER::Convert(value.GetValueUnsafe<T>());
		const U y = comparable - min;
		const U bit_idx = y >> shift;
		const uint8_t in_range = y <= span;
		const uint32_t word_idx = (bit_idx >> WORD_SHIFT) & (0U - in_range);
		const uint8_t bit = (bitmap[word_idx] >> (bit_idx & WORD_MASK)) & 1ULL;
		return bit & in_range;
	}

	template <typename T, typename CONVERTER>
	idx_t LookupKeys(Vector &keys, SelectionVector &result_sel, idx_t count) const {
		idx_t found_count = 0;
		for (const auto &entry : keys.template ValidValues<T>()) {
			const U comparable = CONVERTER::Convert(entry.GetValue());
			const U y = comparable - min;
			const U bit_idx = y >> shift;
			const uint8_t in_range = y <= span;
			const uint32_t word_idx = (bit_idx >> WORD_SHIFT) & (0U - in_range);
			const uint8_t bit = (bitmap[word_idx] >> (bit_idx & WORD_MASK)) & 1ULL;

			result_sel.set_index(found_count, entry.GetIndex());
			found_count += bit & in_range;
		}
		return found_count;
	}

	template <typename T, typename CONVERTER>
	idx_t LookupKeys(Vector &keys, const SelectionVector &sel, SelectionVector &result_sel, idx_t count) const {
		UnifiedVectorFormat key_data;
		keys.ToUnifiedFormat(key_data);

		const auto keys_data = UnifiedVectorFormat::GetData<T>(key_data);
		idx_t found_count = 0;
		for (idx_t i = 0; i < count; i++) {
			const auto idx = sel.get_index_unsafe(i);
			const auto key_idx = key_data.sel->get_index(idx);
			if (!key_data.validity.RowIsValid(key_idx)) {
				continue;
			}
			const U comparable = CONVERTER::Convert(keys_data[key_idx]);
			const U y = comparable - min;
			const U bit_idx = y >> shift;
			const uint8_t in_range = y <= span;
			const uint32_t word_idx = (bit_idx >> WORD_SHIFT) & (0U - in_range);
			const uint8_t bit = (bitmap[word_idx] >> (bit_idx & WORD_MASK)) & 1ULL;

			result_sel.set_index(found_count, i);
			found_count += bit & in_range;
		}
		return found_count;
	}

	FilterPropagateResult LookupRange(U lower_bound, U upper_bound) const {
		const U lb_y = lower_bound - min;
		const U lb_bit_idx = lb_y >> shift;
		const auto lb_word_idx = lb_bit_idx >> WORD_SHIFT;

		const U ub_y = upper_bound - min;
		const U ub_bit_idx = ub_y >> shift;
		const auto ub_word_idx = ub_bit_idx >> WORD_SHIFT;

		const idx_t lb_bit_off = UnsafeNumericCast<idx_t>(lb_bit_idx & UnsafeNumericCast<U>(WORD_MASK));
		const idx_t ub_bit_off = UnsafeNumericCast<idx_t>(ub_bit_idx & UnsafeNumericCast<U>(WORD_MASK));

		// TODO: Count the amount of 1's in the range, compare to a threshold, and make a decision if we want to use the
		// per-row filter for this row group.
		if (lb_word_idx == ub_word_idx) {
			const auto range_mask = ((~0ULL << lb_bit_off) & (~0ULL >> (WORD_MASK - ub_bit_off)));
			if (bitmap[lb_word_idx] & range_mask) {
				return FilterPropagateResult::NO_PRUNING_POSSIBLE;
			}
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}

		const auto lb_word_mask = (~0ULL << lb_bit_off);
		if (bitmap[lb_word_idx] & lb_word_mask) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}

		for (idx_t i = UnsafeNumericCast<idx_t>(lb_word_idx) + 1; i < UnsafeNumericCast<idx_t>(ub_word_idx); i++) {
			if (bitmap[i]) {
				return FilterPropagateResult::NO_PRUNING_POSSIBLE;
			}
		}

		const auto ub_word_mask = ~0ULL >> (WORD_MASK - ub_bit_off);
		if (bitmap[ub_word_idx] & ub_word_mask) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}

		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}

	bool IsInitialized() const {
		return initialized;
	}

	PrefixRangeFilter::Analysis Analyze() const {
		idx_t active_buckets = 0;
		for (idx_t word_idx = 0; word_idx < word_count; word_idx++) {
			active_buckets += UnsafeNumericCast<idx_t>(__builtin_popcountll(bitmap[word_idx]));
		}
		return {active_buckets, PrefixRangeFilter::EstimateFalsePositiveRate(Uhugeint::Convert(span), active_buckets,
		                                                                     active_buckets, shift)};
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

	bool initialized = false;
	U min;
	U span;
	idx_t shift;
	idx_t word_count;
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

uint32_t StringMinComparable(const Value &value) {
	return StringPrefixConverter::Convert(value.GetValueUnsafe<string_t>());
}

uint32_t StringMaxComparable(const Value &value) {
	const auto max_string = value.GetValueUnsafe<string_t>();
	if (max_string.GetSize() >= string_t::PREFIX_BYTES) {
		return max_string.GetPrefixIntegerComparable();
	}

	// Pad string prefix with 0xFF to keep correctness if max is truncated at \0 char, e.g., ab\0c -> ab
	array<char, string_t::PREFIX_BYTES> padded_prefix;
	padded_prefix.fill(char(0xFF));
	for (idx_t i = 0; i < max_string.GetSize(); i++) {
		padded_prefix[i] = max_string.GetData()[i];
	}
	return string_t(padded_prefix.data(), string_t::PREFIX_BYTES).GetPrefixIntegerComparable();
}

template <typename T>
class NumericPrefixRangeFilter : public PrefixRangeFilter {
private:
	using Comparable = typename MakeUnsigned<T>::type;

public:
	void Initialize(ClientContext &context, idx_t number_of_rows, Value min_val, Value max_val,
	                const Sizing &sizing) override {
		D_ASSERT(min_val <= max_val);
		D_ASSERT(number_of_rows > 0);
		const auto min = NumericConverter<T>::Convert(min_val.GetValueUnsafe<T>());
		const auto max = NumericConverter<T>::Convert(max_val.GetValueUnsafe<T>());
		const Comparable comparable_span = max - min;
		Comparable span;
		if (!Uhugeint::TryCast(sizing.span, span) || span != comparable_span) {
			throw InternalException("Prefix range filter sizing does not match its value bounds");
		}
		bitmap.Initialize(context, min, span, sizing.shift);
	}

	unique_ptr<BuildState> InitializeBuildState(ClientContext &context) const override {
		return bitmap.InitializeBuildState(context);
	}

	void InsertKeys(Vector &keys, BuildState &state) const override {
		auto &bitmap_state = state.Cast<PrefixRangeBitmapBuildState>();
		bitmap.template InsertKeys<T, NumericConverter<T>, false>(keys, bitmap_state.bitmap);
	}

	void InsertKeysParallel(Vector &keys, BuildState &state) const override {
		auto &bitmap_state = state.Cast<PrefixRangeBitmapBuildState>();
		bitmap.template InsertKeys<T, NumericConverter<T>, true>(keys, bitmap_state.bitmap);
	}

	void MergeBuildState(BuildState &state) override {
		bitmap.MergeBuildState(state.Cast<PrefixRangeBitmapBuildState>());
	}

	idx_t GetBuildStateSize() const override {
		return bitmap.GetBuildStateSize();
	}

	idx_t LookupKeys(Vector &keys, SelectionVector &result_sel, idx_t count) const override {
		if (keys.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			return bitmap.template LookupOne<T, NumericConverter<T>>(keys.GetValue(0)) ? count : 0;
		}
		return bitmap.template LookupKeys<T, NumericConverter<T>>(keys, result_sel, count);
	}

	idx_t LookupKeys(Vector &keys, const SelectionVector &sel, SelectionVector &result_sel,
	                 idx_t count) const override {
		if (keys.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			return bitmap.template LookupOne<T, NumericConverter<T>>(keys.GetValue(0)) ? count : 0;
		}
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
		return bitmap.LookupRange(adjusted_lb, adjusted_ub);
	}

	bool IsInitialized() const override {
		return bitmap.IsInitialized();
	}

	Analysis Analyze() const override {
		return bitmap.Analyze();
	}

private:
	PrefixRangeBitmap<Comparable> bitmap;
};

class StringPrefixRangeFilter : public PrefixRangeFilter {
public:
	void Initialize(ClientContext &context, idx_t number_of_rows, Value min_val, Value max_val,
	                const Sizing &sizing) override {
		D_ASSERT(min_val <= max_val);
		D_ASSERT(number_of_rows > 0);
		const auto min = StringPrefixConverter::Convert(min_val.GetValueUnsafe<string_t>());
		const auto max = StringPrefixConverter::Convert(max_val.GetValueUnsafe<string_t>());
		D_ASSERT(min <= max);
		uint32_t span;
		if (!Uhugeint::TryCast(sizing.span, span) || span != max - min) {
			throw InternalException("Prefix range filter sizing does not match its value bounds");
		}
		bitmap.Initialize(context, min, span, sizing.shift);
	}

	unique_ptr<BuildState> InitializeBuildState(ClientContext &context) const override {
		return bitmap.InitializeBuildState(context);
	}

	void InsertKeys(Vector &keys, BuildState &state) const override {
		auto &bitmap_state = state.Cast<PrefixRangeBitmapBuildState>();
		bitmap.template InsertKeys<string_t, StringPrefixConverter, false>(keys, bitmap_state.bitmap);
	}

	void InsertKeysParallel(Vector &keys, BuildState &state) const override {
		auto &bitmap_state = state.Cast<PrefixRangeBitmapBuildState>();
		bitmap.template InsertKeys<string_t, StringPrefixConverter, true>(keys, bitmap_state.bitmap);
	}

	void MergeBuildState(BuildState &state) override {
		bitmap.MergeBuildState(state.Cast<PrefixRangeBitmapBuildState>());
	}

	idx_t GetBuildStateSize() const override {
		return bitmap.GetBuildStateSize();
	}

	idx_t LookupKeys(Vector &keys, SelectionVector &result_sel, idx_t count) const override {
		if (keys.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			return bitmap.template LookupOne<string_t, StringPrefixConverter>(keys.GetValue(0)) ? count : 0;
		}
		return bitmap.template LookupKeys<string_t, StringPrefixConverter>(keys, result_sel, count);
	}

	idx_t LookupKeys(Vector &keys, const SelectionVector &sel, SelectionVector &result_sel,
	                 idx_t count) const override {
		if (keys.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			return bitmap.template LookupOne<string_t, StringPrefixConverter>(keys.GetValue(0)) ? count : 0;
		}
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
		return bitmap.LookupRange(lower_bound_comparable, upper_bound_comparable);
	}

	bool IsInitialized() const override {
		return bitmap.IsInitialized();
	}

	Analysis Analyze() const override {
		return bitmap.Analyze();
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

idx_t MaximumPrefixRangeShift(const LogicalType &type) {
	if (type.InternalType() == PhysicalType::VARCHAR) {
		return sizeof(uint32_t) * 8 - 1;
	}
	return GetTypeIdSize(type.InternalType()) * 8 - 1;
}

bool PrefixRangeFilter::TryComputeBucketCount(const uhugeint_t &span, idx_t shift, idx_t &bucket_count) {
	if (shift >= 64) {
		return false;
	}
	const auto shifted_span = span >> uhugeint_t(shift);
	auto bucket_count_huge = shifted_span;
	if (!Uhugeint::TryAddInPlace(bucket_count_huge, 1)) {
		return false;
	}
	return Uhugeint::TryCast(bucket_count_huge, bucket_count);
}

double PrefixRangeFilter::EstimateFalsePositiveRate(const uhugeint_t &span, idx_t positive_lower_bound,
                                                    idx_t active_buckets, idx_t shift) {
	const auto domain_size = span + 1;
	const auto represented_values = Uhugeint::Convert(positive_lower_bound);
	if (Uhugeint::LessThanEquals(domain_size, represented_values)) {
		return 0;
	}

	D_ASSERT(shift < 64);
	const auto bucket_width = uhugeint_t(1) << uhugeint_t(shift);
	auto covered_values = Uhugeint::Convert(active_buckets) * bucket_width;
	if (Uhugeint::GreaterThan(covered_values, domain_size)) {
		covered_values = domain_size;
	}
	if (Uhugeint::LessThanEquals(covered_values, represented_values)) {
		return 0;
	}

	const auto false_positives = covered_values - represented_values;
	const auto negative_values = domain_size - represented_values;
	return Uhugeint::Cast<double>(false_positives) / Uhugeint::Cast<double>(negative_values);
}

double PrefixRangeFilter::ComputeFalsePositiveRateUpperBound(const uhugeint_t &span, idx_t count, idx_t shift) {
	return EstimateFalsePositiveRate(span, count, count, shift);
}

bool PrefixRangeFilter::TryComputeSizing(const Value &min, const Value &max, idx_t count, Sizing &sizing,
                                         double false_positive_rate) {
	if (count == 0 || false_positive_rate < 0 || !TryComputeSpan(min, max, sizing.span)) {
		return false;
	}

	sizing.shift = 0;
	bool found_sizing = false;
	const auto maximum_shift = MaximumPrefixRangeShift(min.type());
	for (idx_t shift = 0; shift <= maximum_shift; shift++) {
		idx_t bucket_count;
		if (!TryComputeBucketCount(sizing.span, shift, bucket_count)) {
			continue;
		}
		if (ComputeFalsePositiveRateUpperBound(sizing.span, count, shift) > false_positive_rate) {
			break;
		}
		sizing.shift = shift;
		found_sizing = true;
	}
	return found_sizing;
}

bool PrefixRangeFilter::TryComputeFixedSizeSizing(const Value &min, const Value &max, idx_t bucket_count_limit,
                                                  Sizing &sizing) {
	if (bucket_count_limit == 0 || !TryComputeSpan(min, max, sizing.span)) {
		return false;
	}

	const auto maximum_shift = MaximumPrefixRangeShift(min.type());
	for (idx_t shift = 0; shift <= maximum_shift; shift++) {
		idx_t bucket_count;
		if (TryComputeBucketCount(sizing.span, shift, bucket_count) && bucket_count <= bucket_count_limit) {
			sizing.shift = shift;
			return true;
		}
	}
	return false;
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

LegacyPrefixRangeTableFilter::LegacyPrefixRangeTableFilter(optional_ptr<PrefixRangeFilter> filter_p,
                                                           const string &key_column_name_p,
                                                           const LogicalType &key_type_p)
    : TableFilter(TYPE), filter(filter_p), key_column_name(key_column_name_p), key_type(key_type_p) {
}

unique_ptr<Expression> LegacyPrefixRangeTableFilter::ToExpression(const Expression &column) const {
	auto function = PrefixRangeScalarFun::GetFunction(column.GetReturnType());
	auto bind_data = make_uniq<PrefixRangeFunctionData>(filter, true, key_column_name, key_type, 0.0f, idx_t(0));
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(column.Copy());
	return make_uniq<BoundFunctionExpression>(BoundScalarFunction(function), std::move(arguments),
	                                          std::move(bind_data));
}

void LegacyPrefixRangeTableFilter::Serialize(Serializer &serializer) const {
	TableFilter::Serialize(serializer);
	serializer.WriteProperty<string>(200, "key_column_name", key_column_name);
	serializer.WriteProperty<LogicalType>(201, "key_type", key_type);
}

unique_ptr<TableFilter> LegacyPrefixRangeTableFilter::Deserialize(Deserializer &deserializer) {
	auto key_column_name = deserializer.ReadProperty<string>(200, "key_column_name");
	auto key_type = deserializer.ReadProperty<LogicalType>(201, "key_type");

	auto result = make_uniq<LegacyPrefixRangeTableFilter>(nullptr, key_column_name, key_type);
	return std::move(result);
}

} // namespace duckdb
