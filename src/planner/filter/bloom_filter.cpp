#include "duckdb/planner/filter/bloom_filter.hpp"

#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"

namespace duckdb {

unique_ptr<Expression> LegacyBFTableFilter::ToExpression(const Expression &column) const {
	auto function = BloomFilterScalarFun::GetFunction(column.GetReturnType());
	auto bind_data =
	    make_uniq<BloomFilterFunctionData>(filter, filters_null_values, key_column_name, key_type, 0.0f, idx_t(0));
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(column.Copy());
	return make_uniq<BoundFunctionExpression>(BoundScalarFunction(function), std::move(arguments),
	                                          std::move(bind_data));
}

inline uint64_t GetMask(const hash_t hash) {
	const uint64_t shifts = hash & SHIFT_MASK;
	const auto shifts_8 = reinterpret_cast<const uint8_t *>(&shifts);

	uint64_t mask = 0;

	for (idx_t bit_idx = 8 - N_BITS; bit_idx < 8; bit_idx++) {
		const uint8_t bit_pos = shifts_8[bit_idx];
		mask |= (1ULL << bit_pos);
	}

	return mask;
}

void BloomFilter::InsertHashes(const Vector &hashes_v, idx_t count) const {
	auto hashes = FlatVector::GetData<uint64_t>(hashes_v);
	for (idx_t i = 0; i < count; i++) {
		InsertOne(hashes[i]);
	}
}

idx_t BloomFilter::LookupHashes(const Vector &hashes_v, SelectionVector &result_sel, const idx_t count) const {
	D_ASSERT(hashes_v.GetVectorType() == VectorType::FLAT_VECTOR);
	D_ASSERT(hashes_v.GetType() == LogicalType::HASH);

	const auto hashes = FlatVector::GetData<uint64_t>(hashes_v);
	idx_t found_count = 0;
	for (idx_t i = 0; i < count; i++) {
		result_sel.set_index(found_count, i);
		found_count += LookupOne(hashes[i]);
	}
	return found_count;
}

inline void BloomFilter::InsertOne(const hash_t hash) const {
	D_ASSERT(initialized);
	const uint64_t bf_offset = hash & bitmask;
	const uint64_t mask = GetMask(hash);
	atomic<uint64_t> &slot = *reinterpret_cast<atomic<uint64_t> *>(&bf[bf_offset]);

	slot.fetch_or(mask, std::memory_order_relaxed);
}

inline bool BloomFilter::LookupOne(const uint64_t hash) const {
	D_ASSERT(initialized);
	const uint64_t bf_offset = hash & bitmask;
	const uint64_t mask = GetMask(hash);
	atomic<uint64_t> &slot = *reinterpret_cast<atomic<uint64_t> *>(&bf[bf_offset]);
	auto bf_entry = slot.load(std::memory_order_relaxed);

	return (bf_entry & mask) == mask;
}

string BFTableFilter::ToString(const string &column_name) const {
	return column_name + " IN BF(" + key_column_name + ")";
}

idx_t BFTableFilter::Filter(Vector &keys_v, SelectionVector &sel, idx_t &approved_tuple_count,
                            JoinFilterTableFilterState &state) const {
	if (!filter.IsInitialized()) {
		return approved_tuple_count;
	}
	state.PrepareSlicedKeys(keys_v, sel, approved_tuple_count);
	VectorOperations::Hash(state.keys_sliced_v, state.hashes_v, approved_tuple_count);

	idx_t found_count;
	if (state.hashes_v.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		const auto constant_hash = *ConstantVector::GetData<hash_t>(state.hashes_v);
		const bool found = this->filter.LookupOne(constant_hash);
		found_count = found ? approved_tuple_count : 0;
	} else {
		state.hashes_v.Flatten(approved_tuple_count);
		found_count = this->filter.LookupHashes(state.hashes_v, state.probe_sel, approved_tuple_count);
	}

	if (found_count == approved_tuple_count) {
		return approved_tuple_count;
	}

	if (sel.IsSet()) {
		for (idx_t idx = 0; idx < found_count; idx++) {
			const idx_t flat_sel_idx = state.probe_sel.get_index(idx);
			const idx_t original_sel_idx = sel.get_index(flat_sel_idx);
			sel.set_index(idx, original_sel_idx);
		}
	} else {
		sel.Initialize(state.probe_sel);
	}

	approved_tuple_count = found_count;
	return approved_tuple_count;
}

bool BFTableFilter::FilterValue(const Value &value) const {
	if (!filter.IsInitialized()) {
		return true;
	}
	const auto hash = value.Hash();
	return filter.LookupOne(hash);
}

template <class T>
static FilterPropagateResult TemplatedCheckStatistics(const BloomFilter &bf, T min, T max) {
	if (min > max) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	T range_typed;
	idx_t range;
	if (!TrySubtractOperator::Operation(max, min, range_typed) || !TryCast::Operation(range_typed, range) ||
	    range >= DEFAULT_STANDARD_VECTOR_SIZE) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}

	T val = min;
	idx_t hits = 0;
	for (idx_t i = 0; i <= range; i++) {
		hits += bf.LookupOne(Hash(val));
		val += i < range;
	}

	if (hits == 0) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	if (hits == range + 1) {
		return FilterPropagateResult::FILTER_ALWAYS_TRUE;
	}
	return FilterPropagateResult::NO_PRUNING_POSSIBLE;
}

FilterPropagateResult BFTableFilter::CheckStatistics(BaseStatistics &stats) const {
	if (!filter.IsInitialized()) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (!TypeIsInteger(key_type.InternalType()) || !NumericStats::HasMinMax(stats)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}

	auto min_val = NumericStats::Min(stats);
	auto max_val = NumericStats::Max(stats);

	if (stats.GetType() != key_type && (!min_val.DefaultTryCastAs(key_type) || !max_val.DefaultTryCastAs(key_type))) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}

	switch (key_type.InternalType()) {
	case PhysicalType::UINT8:
		return TemplatedCheckStatistics<uint8_t>(filter, min_val.GetValueUnsafe<uint8_t>(),
		                                         max_val.GetValueUnsafe<uint8_t>());
	case PhysicalType::UINT16:
		return TemplatedCheckStatistics<uint16_t>(filter, min_val.GetValueUnsafe<uint16_t>(),
		                                          max_val.GetValueUnsafe<uint16_t>());
	case PhysicalType::UINT32:
		return TemplatedCheckStatistics<uint32_t>(filter, min_val.GetValueUnsafe<uint32_t>(),
		                                          max_val.GetValueUnsafe<uint32_t>());
	case PhysicalType::UINT64:
		return TemplatedCheckStatistics<uint64_t>(filter, min_val.GetValueUnsafe<uint64_t>(),
		                                          max_val.GetValueUnsafe<uint64_t>());
	case PhysicalType::UINT128:
		return TemplatedCheckStatistics<uhugeint_t>(filter, min_val.GetValueUnsafe<uhugeint_t>(),
		                                            max_val.GetValueUnsafe<uhugeint_t>());
	case PhysicalType::INT8:
		return TemplatedCheckStatistics<int8_t>(filter, min_val.GetValueUnsafe<int8_t>(),
		                                        max_val.GetValueUnsafe<int8_t>());
	case PhysicalType::INT16:
		return TemplatedCheckStatistics<int16_t>(filter, min_val.GetValueUnsafe<int16_t>(),
		                                         max_val.GetValueUnsafe<int16_t>());
	case PhysicalType::INT32:
		return TemplatedCheckStatistics<int32_t>(filter, min_val.GetValueUnsafe<int32_t>(),
		                                         max_val.GetValueUnsafe<int32_t>());
	case PhysicalType::INT64:
		return TemplatedCheckStatistics<int64_t>(filter, min_val.GetValueUnsafe<int64_t>(),
		                                         max_val.GetValueUnsafe<int64_t>());
	case PhysicalType::INT128:
		return TemplatedCheckStatistics<hugeint_t>(filter, min_val.GetValueUnsafe<hugeint_t>(),
		                                           max_val.GetValueUnsafe<hugeint_t>());
	default:
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
}

bool BFTableFilter::Equals(const TableFilter &other_p) const {
	if (!TableFilter::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<BFTableFilter>();
	return RefersToSameObject(filter, other.filter) && filters_null_values == other.filters_null_values &&
	       key_column_name == other.key_column_name && key_type == other.key_type;
}

unique_ptr<TableFilter> BFTableFilter::Copy() const {
	return make_uniq<BFTableFilter>(this->filter, this->filters_null_values, this->key_column_name, this->key_type);
}

unique_ptr<Expression> BFTableFilter::ToExpression(const Expression &column) const {
	auto bound_constant = make_uniq<BoundConstantExpression>(Value(true));
	return std::move(bound_constant);
}

void BFTableFilter::Serialize(Serializer &serializer) const {
	TableFilter::Serialize(serializer);
	serializer.WriteProperty<bool>(200, "filters_null_values", filters_null_values);
	serializer.WriteProperty<string>(201, "key_column_name", key_column_name);
	serializer.WriteProperty<LogicalType>(202, "key_type", key_type);
}

void LegacyBFTableFilter::Serialize(Serializer &serializer) const {
	TableFilter::Serialize(serializer);
	serializer.WriteProperty<bool>(200, "filters_null_values", filters_null_values);
	serializer.WriteProperty<string>(201, "key_column_name", key_column_name);
	serializer.WriteProperty<LogicalType>(202, "key_type", key_type);
}

unique_ptr<TableFilter> LegacyBFTableFilter::Deserialize(Deserializer &deserializer) {
	auto filters_null_values = deserializer.ReadProperty<bool>(200, "filters_null_values");
	auto key_column_name = deserializer.ReadProperty<string>(201, "key_column_name");
	auto key_type = deserializer.ReadProperty<LogicalType>(202, "key_type");

	auto result = make_uniq<LegacyBFTableFilter>(nullptr, filters_null_values, key_column_name, key_type);
	return std::move(result);
}

} // namespace duckdb
