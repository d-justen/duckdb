//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/filter/prefix_range_filter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.h"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/table_filter_state.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include <cstdint>

namespace duckdb {

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
};

template <typename T>
class NumericPrefixRangeFilter : public PrefixRangeFilter {
public:
	void Initialize(ClientContext &context, idx_t number_of_rows, Value min, Value max) override {
		static constexpr idx_t KEY_BIT_SIZE = sizeof(T) * 8;
		static constexpr idx_t PREFIX_LENGTH = 18;
		static constexpr idx_t BITMAP_SIZE = 1 << (PREFIX_LENGTH - 6); // = 2^PREFIX_LENGTH / sizeof(uint64_t)

		prefix_shift = KEY_BIT_SIZE - PREFIX_LENGTH;
		word_shift = prefix_shift + 6;

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
			bitmap[(idx_t)key >> word_shift] |= 1ULL << (((idx_t)key >> prefix_shift) & 63);
		}
	}

	void InsertOne(const Value &key) const override {
		auto k = key.GetValueUnsafe<T>();
		bitmap[(idx_t)k >> word_shift] |= 1ULL << (((idx_t)k >> prefix_shift) & 63);
	}

	idx_t LookupKeys(const Vector &keys, SelectionVector &result_sel, idx_t count) const override {
		const T *key_data = FlatVector::GetData<T>(keys);
		idx_t found_count = 0;
		for (idx_t i = 0; i < count; i++) {
			result_sel.set_index(found_count, i);
			T key = key_data[i];
			found_count += (bool)bitmap[(idx_t)key >> word_shift] & 1ULL << (((idx_t)key >> prefix_shift) & 64);
		}
		return found_count;
	}

	bool LookupOne(const Value &key) const override {
		auto k = key.GetValueUnsafe<T>();
		return bitmap[(idx_t)k >> word_shift] & 1ULL << (((idx_t)k >> prefix_shift) & 63);
	}

	idx_t LookupRange(const Value &lower_bound, const Value &upper_bound) const override {
		auto lb = lower_bound.GetValueUnsafe<T>();
		auto ub = upper_bound.GetValueUnsafe<T>();
		idx_t result_count = 0;
		// Bits to the left: larger (important to lower bound)
		// Bits to the right: smaller (important to uppper bound)
		// Shift position in word to last bit to exclude everything to the right
		// TODO: Test this
		result_count += __builtin_popcount(bitmap[(idx_t)lb >> word_shift] >> (((idx_t)ub >> prefix_shift) & 63));
		// Shift the position in word to first bit to check if any of the following bits in word are 1
		// TODO: Test this
		result_count += __builtin_popcount(bitmap[(idx_t)ub >> word_shift]
		                                   << (sizeof(uint64_t) - (((idx_t)ub >> prefix_shift) & 63)));

		for (auto i = lb + 1; i < ub; i += 1) {
			result_count += __builtin_popcount(bitmap[(idx_t)i]);
		}
		return result_count;
	}

	bool IsInitialized() const override {
		return initialized;
	}

	unique_ptr<PrefixRangeFilter> Copy() const override {
		D_ASSERT(!initialized);
		return make_uniq<NumericPrefixRangeFilter<T>>();
	}

private:
	idx_t word_shift;
	idx_t prefix_shift;

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
