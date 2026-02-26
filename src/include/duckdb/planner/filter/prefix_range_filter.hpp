//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/filter/prefix_range_filter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/table_filter_state.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include <type_traits>

namespace duckdb {

class PrefixRangeFilter;

template <typename T, unsigned int N>
class TinyPrefixRangeFilter;

template <typename T>
unique_ptr<PrefixRangeFilter> TryCreateTinyPrefixRangeFilter(ClientContext &context, const uint64_t *bitmap,
                                                             idx_t bitmap_length, typename MakeUnsigned<T>::type offset,
                                                             typename MakeUnsigned<T>::type bucket_size);

class PrefixRangeFilter {
public:
	virtual ~PrefixRangeFilter() = default;
	virtual void Initialize(ClientContext &context, idx_t number_of_rows, Value min, Value max) = 0;
	virtual void InsertKeys(const Vector &keys, idx_t count) const = 0;
	virtual void InsertOne(const Value &key) const = 0;
	virtual idx_t LookupKeys(const Vector &keys, SelectionVector &result_sel, idx_t count) const = 0;
	virtual bool LookupOne(const Value &key) const = 0;
	virtual idx_t LookupRange(const Value &lower_bound, const Value &upper_bound) const = 0;
	virtual bool IsInitialized() const = 0;
	virtual unique_ptr<PrefixRangeFilter> Copy() const = 0;
	virtual void Print() const = 0;
	virtual unique_ptr<PrefixRangeFilter> TryConvertToTiny(ClientContext &context) const {
		return nullptr;
	}
};

template <typename T>
class NumericPrefixRangeFilter : public PrefixRangeFilter {
public:
	static constexpr idx_t KEY_BIT_SIZE = sizeof(T) * 8;
	static constexpr idx_t PREFIX_LENGTH = 18;
	static constexpr idx_t BITMAP_SIZE = 1 << (PREFIX_LENGTH - 6); // = 2^PREFIX_LENGTH / sizeof(uint64_t)
public:
	void Initialize(ClientContext &context, idx_t number_of_rows, Value min, Value max) override {
		using UT = typename MakeUnsigned<T>::type;
		auto min_val = static_cast<UT>(min.GetValueUnsafe<T>());
		auto max_val = static_cast<UT>(max.GetValueUnsafe<T>());
		idx_t prefix_overlap = 0;

		for (int i = sizeof(min_val) * 8 - 1; i >= 0; --i) {
			if ((min_val >> i) == (max_val >> i)) {
				++prefix_overlap;
			} else {
				break;
			}
		}

		prefix_shift = KEY_BIT_SIZE - PREFIX_LENGTH;
		word_shift = prefix_shift + 6;
		overlap_shift = prefix_overlap;
		if (overlap_shift == 0) {
			prefix_base = 0;
		} else {
			prefix_base = min_val >> (KEY_BIT_SIZE - overlap_shift);
		}

		BufferManager &buffer_manager = BufferManager::GetBufferManager(context);
		buf_ = buffer_manager.GetBufferAllocator().Allocate(64ULL + BITMAP_SIZE * sizeof(uint64_t));
		bitmap = reinterpret_cast<uint64_t *>((64ULL + reinterpret_cast<uint64_t>(buf_.get())) & ~63ULL);
		std::fill_n(bitmap, BITMAP_SIZE, 0);

		initialized = true;
	}

	void InsertKeys(const Vector &keys, idx_t count) const override {
		const T *key_data = FlatVector::GetData<T>(keys);
		for (idx_t i = 0; i < count; i++) {
			T key = key_data[i];
			auto stripped = key << overlap_shift;
			bitmap[(idx_t)(stripped >> word_shift)] |= 1ULL << (((idx_t)(stripped >> prefix_shift)) & 63);
		}
	}

	void InsertOne(const Value &key) const override {
		auto k = key.GetValueUnsafe<T>();
		auto stripped = k << overlap_shift;
		bitmap[(idx_t)stripped >> word_shift] |= 1ULL << (((idx_t)stripped >> prefix_shift) & 63);
	}

	idx_t LookupKeys(const Vector &keys, SelectionVector &result_sel, idx_t count) const override {
		const T *key_data = FlatVector::GetData<T>(keys);
		idx_t found_count = 0;
		for (idx_t i = 0; i < count; i++) {
			result_sel.set_index(found_count, i);
			T key = key_data[i];
			auto stripped = key << overlap_shift;
			const auto bitset = bitmap[(idx_t)(stripped >> word_shift)];
			const auto bit = 1ULL << (((idx_t)(stripped >> prefix_shift)) & 63);
			found_count += (bitset & bit) != 0;
		}
		return found_count;
	}

	bool LookupOne(const Value &key) const override {
		auto k = key.GetValueUnsafe<T>();
		auto stripped = k << overlap_shift;
		return bitmap[(idx_t)(stripped >> word_shift)] & 1ULL << (((idx_t)(stripped >> prefix_shift)) & 63);
	}

	idx_t LookupRange(const Value &lower_bound, const Value &upper_bound) const override {
		auto lb = lower_bound.GetValueUnsafe<T>();
		auto ub = upper_bound.GetValueUnsafe<T>();
		const auto lower_prefix_stripped = lb << overlap_shift;
		const auto upper_prefix_stripped = ub << overlap_shift;
		const auto lower_prefix_word_idx = lower_prefix_stripped >> word_shift;
		const auto upper_prefix_word_idx = upper_prefix_stripped >> word_shift;
		const auto lower_prefix_bit_idx = (lower_prefix_stripped >> prefix_shift) & 63;
		const auto upper_prefix_bit_idx = (upper_prefix_stripped >> prefix_shift) & 63;

		auto lb_word = bitmap[(idx_t)lower_prefix_word_idx];
		auto lb_mask = ~0ULL << (uint64_t)lower_prefix_bit_idx;
		if (lb_word & lb_mask) {
			return 1;
		}

		for (idx_t i = (idx_t)lower_prefix_word_idx; i < (idx_t)upper_prefix_word_idx; i++) {
			if (bitmap[i] > 0) {
				return 1;
			}
		}

		auto ub_word = bitmap[(idx_t)upper_prefix_word_idx];
		auto ub_mask = (1ULL << (uint64_t)upper_prefix_bit_idx) - 1;
		if (ub_word & ub_mask) {
			return 1;
		}
		return 0;
	}

	bool IsInitialized() const override {
		return initialized;
	}

	unique_ptr<PrefixRangeFilter> TryConvertToTiny(ClientContext &context) const override;

	unique_ptr<PrefixRangeFilter> Copy() const override {
		auto result = make_uniq<NumericPrefixRangeFilter<T>>();
		if (!initialized) {
			return std::move(result);
		}

		static constexpr idx_t PREFIX_LENGTH = 18;
		static constexpr idx_t BITMAP_SIZE = 1 << (PREFIX_LENGTH - 6);
		result->overlap_shift = overlap_shift;
		result->word_shift = word_shift;
		result->prefix_shift = prefix_shift;
		result->prefix_base = prefix_base;
		result->buf_ = Allocator::DefaultAllocator().Allocate(64ULL + BITMAP_SIZE * sizeof(uint64_t));
		result->bitmap =
		    reinterpret_cast<uint64_t *>((64ULL + reinterpret_cast<uint64_t>(result->buf_.get())) & ~63ULL);
		memcpy(result->bitmap, bitmap, BITMAP_SIZE * sizeof(uint64_t));
		result->initialized = true;
		return std::move(result);
	}

	void Print() const override {
		std::cout << "[";
		for (idx_t i = 0; i < BITMAP_SIZE; i++) {
			if (bitmap[i] == 0) {
				std::cout << "0";
			} else {
				std::cout << "{" << bitmap[i] << "}";
			}
		}
		std::cout << "]\n";
	}

private:
	template <typename U, typename std::enable_if<(sizeof(U) <= sizeof(uint64_t)), int>::type = 0>
	static unique_ptr<PrefixRangeFilter> TryConvertToTinyInternal(const NumericPrefixRangeFilter<U> &filter,
	                                                              ClientContext &context) {
		using UT = typename MakeUnsigned<U>::type;
		if (!filter.initialized) {
			return nullptr;
		}
		if (KEY_BIT_SIZE <= PREFIX_LENGTH) {
			return nullptr;
		}
		if (filter.overlap_shift > filter.prefix_shift) {
			return nullptr;
		}
		const auto shift = filter.prefix_shift - filter.overlap_shift;
		const auto bucket_size = static_cast<UT>(UT(1) << shift);
		const auto offset = static_cast<UT>(filter.prefix_base << (KEY_BIT_SIZE - filter.overlap_shift));
		return TryCreateTinyPrefixRangeFilter<U>(context, filter.bitmap, BITMAP_SIZE, offset, bucket_size);
	}

	template <typename U, typename std::enable_if<(sizeof(U) > sizeof(uint64_t)), int>::type = 0>
	static unique_ptr<PrefixRangeFilter> TryConvertToTinyInternal(const NumericPrefixRangeFilter<U> &filter,
	                                                              ClientContext &context) {
		return nullptr;
	}

	idx_t word_shift;
	idx_t prefix_shift;
	idx_t overlap_shift;
	typename MakeUnsigned<T>::type prefix_base;

	bool initialized = false;
	AllocatedData buf_;
	uint64_t *bitmap;
};

class PrefixRangeTableFilter final : public TableFilter {
private:
	unique_ptr<PrefixRangeFilter> filter;

	bool filters_null_values;
	string key_column_name;
	LogicalType key_type;

public:
	static constexpr auto TYPE = TableFilterType::PREFIX_RANGE_FILTER;
	static bool SupportedType(const LogicalType &type);
	static unique_ptr<PrefixRangeFilter> CreateFilter(const LogicalType &type);

public:
	explicit PrefixRangeTableFilter(unique_ptr<PrefixRangeFilter> filter_p, const bool filters_null_values_p,
	                                const string &key_column_name_p, const LogicalType &key_type_p);

	//! If the join condition is e.g. "A = B", the bf will filter null values.
	//! If the condition is "A is B" the filter will let nulls pass
	bool FiltersNullValues() const {
		return filters_null_values;
	}

	LogicalType GetKeyType() const {
		return key_type;
	}

	string ToString(const string &column_name) const override;

	idx_t Filter(Vector &keys, SelectionVector &sel, idx_t &approved_tuple_count,
	             PrefixRangeTableFilterState &state) const;
	bool FilterValue(const Value &value) const;

	FilterPropagateResult CheckStatistics(BaseStatistics &stats) const override;

private:
	bool Equals(const TableFilter &other) const override;
	unique_ptr<TableFilter> Copy() const override;
	unique_ptr<Expression> ToExpression(const Expression &column) const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<TableFilter> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb

#include "duckdb/planner/filter/tiny_prefix_range_filter.hpp"

namespace duckdb {

template <typename T>
unique_ptr<PrefixRangeFilter> NumericPrefixRangeFilter<T>::TryConvertToTiny(ClientContext &context) const {
	return TryConvertToTinyInternal(*this, context);
}

} // namespace duckdb
