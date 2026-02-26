
//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/filter/tiny_prefix_range_filter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/filter/prefix_range_filter.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include <cstddef>
#include <cstdint>

namespace duckdb {

template <typename T, unsigned int N>
class TinyPrefixRangeFilter : public PrefixRangeFilter {
public:
	static constexpr idx_t TINY_THRESHOLD = 8;

	TinyPrefixRangeFilter() : lower_bounds(nullptr), upper_bounds(nullptr) {
	}

	TinyPrefixRangeFilter(ClientContext &context, const vector<T> &lowers, const vector<T> &uppers) {
		if (lowers.size() != N || uppers.size() != N) {
			throw InternalException("Expected %d ranges to be tiny", N);
		}

		BufferManager &buffer_manager = BufferManager::GetBufferManager(context);
		buf_ = buffer_manager.GetBufferAllocator().Allocate(64ULL + static_cast<idx_t>(2 * N) * sizeof(T));
		lower_bounds = reinterpret_cast<T *>((64ULL + reinterpret_cast<uint64_t>(buf_.get())) & ~63ULL);
		upper_bounds = lower_bounds + N;
		memcpy(lower_bounds, lowers.data(), N * sizeof(T));
		memcpy(upper_bounds, uppers.data(), N * sizeof(T));
	}

	void Initialize(ClientContext &context, idx_t number_of_rows, Value min, Value max) override {
	}

	void InsertKeys(const Vector &keys, idx_t count) const override {
	}

	void InsertOne(const Value &key) const override {
	}

	idx_t LookupKeys(const Vector &keys, SelectionVector &result_sel, idx_t count) const override {
		const T *key_data = FlatVector::GetData<T>(keys);
		idx_t found_count = 0;
		for (idx_t i = 0; i < count; i++) {
			result_sel.set_index(found_count, i);
			T key = key_data[i];
			bool found = false;
			for (idx_t range_idx = 0; range_idx < N; range_idx++) {
				found |= (key >= lower_bounds[range_idx]) & (key <= upper_bounds[range_idx]);
			}
			found_count += found;
		}
		return found_count;
	}

	bool LookupOne(const Value &key) const override {
		auto key_data = key.GetValueUnsafe<T>();
		bool found = false;
		for (idx_t i = 0; i < N; i++) {
			found = found || (key_data >= lower_bounds[i] && key_data <= upper_bounds[i]);
		}
		return found;
	}

	idx_t LookupRange(const Value &lower_bound, const Value &upper_bound) const override {
		auto lb = lower_bound.GetValueUnsafe<T>();
		auto ub = upper_bound.GetValueUnsafe<T>();

		for (idx_t i = 0; i < N; i++) {
			if (lb <= upper_bounds[i] && lower_bounds[i] <= ub) {
				return 1;
			}
		}
		return 0;
	}

	bool IsInitialized() const override {
		return true;
	}

	unique_ptr<PrefixRangeFilter> Copy() const override {
		auto result = make_uniq<TinyPrefixRangeFilter<T, N>>();

		result->buf_ = Allocator::DefaultAllocator().Allocate(64ULL + NumericCast<idx_t>(2 * N) * sizeof(T));
		result->lower_bounds = reinterpret_cast<T *>((64ULL + reinterpret_cast<uint64_t>(result->buf_.get())) & ~63ULL);
		result->upper_bounds = result->lower_bounds + N;
		memcpy(result->lower_bounds, lower_bounds, N * sizeof(T));
		memcpy(result->upper_bounds, upper_bounds, N * sizeof(T));
		return std::move(result);
	}

	void Print() const override {
	}

private:
	AllocatedData buf_;
	T *lower_bounds;
	T *upper_bounds;
};

template <typename T>
unique_ptr<PrefixRangeFilter> TryCreateTinyPrefixRangeFilter(ClientContext &context, const uint64_t *bitmap,
                                                             idx_t bitmap_length, typename MakeUnsigned<T>::type offset,
                                                             typename MakeUnsigned<T>::type bucket_size) {
	vector<T> lowers;
	vector<T> uppers;

	bool open = false;
	idx_t start_bit = 0;
	idx_t total_bits = bitmap_length * 64;

	for (idx_t bit = 0; bit < total_bits; ++bit) {
		uint64_t word = bitmap[bit >> 6];
		bool set = (word >> (bit & 63)) & 1ull;

		if (set && !open) {
			open = true;
			start_bit = bit;
		}

		if (!set && open) {
			idx_t end_bit = bit - 1;
			using UT = typename MakeUnsigned<T>::type;
			auto lower_idx = static_cast<UT>(offset + static_cast<UT>(start_bit) * bucket_size);
			auto upper_idx = static_cast<UT>(offset + static_cast<UT>(end_bit + 1) * bucket_size - 1);
			T lower = static_cast<T>(lower_idx);
			T upper = static_cast<T>(upper_idx);

			lowers.push_back(lower);
			uppers.push_back(upper);
			if (lowers.size() > TinyPrefixRangeFilter<T, 1>::TINY_THRESHOLD) {
				return nullptr;
			}
			open = false;
		}
	}

	if (open) {
		idx_t end_bit = total_bits - 1;
		using UT = typename MakeUnsigned<T>::type;
		auto lower_idx = static_cast<UT>(offset + static_cast<UT>(start_bit) * bucket_size);
		auto upper_idx = static_cast<UT>(offset + static_cast<UT>(end_bit + 1) * bucket_size - 1);
		T lower = static_cast<T>(lower_idx);
		T upper = static_cast<T>(upper_idx);

		lowers.push_back(lower);
		uppers.push_back(upper);
		if (lowers.size() > TinyPrefixRangeFilter<T, 1>::TINY_THRESHOLD) {
			return nullptr;
		}
	}

	if (lowers.size() != uppers.size()) {
		throw InternalException("Lower/upper range vector size mismatch");
	}

	if (lowers.size() <= 8) {
		std::cout << "Tinyfy!\n";
	} else {
		std::cout << " No tinify :( " << lowers.size() << "\n";
	}

	switch (lowers.size()) {
	case 1:
		return make_uniq<TinyPrefixRangeFilter<T, 1>>(context, lowers, uppers);
	case 2:
		return make_uniq<TinyPrefixRangeFilter<T, 2>>(context, lowers, uppers);
	case 3:
		return make_uniq<TinyPrefixRangeFilter<T, 3>>(context, lowers, uppers);
	case 4:
		return make_uniq<TinyPrefixRangeFilter<T, 4>>(context, lowers, uppers);
	case 5:
		return make_uniq<TinyPrefixRangeFilter<T, 5>>(context, lowers, uppers);
	case 6:
		return make_uniq<TinyPrefixRangeFilter<T, 6>>(context, lowers, uppers);
	case 7:
		return make_uniq<TinyPrefixRangeFilter<T, 7>>(context, lowers, uppers);
	case 8:
		return make_uniq<TinyPrefixRangeFilter<T, 8>>(context, lowers, uppers);
	default:
		return nullptr;
	}
}

} // namespace duckdb
