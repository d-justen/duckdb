#include "duckdb/planner/filter/prefix_range_filter.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/common/enums/filter_propagate_result.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

namespace duckdb {

unique_ptr<PrefixRangeFilter> PrefixRangeTableFilter::CreateFilter(const LogicalType &type) {
	switch (type.InternalType()) {
	case PhysicalType::UINT8:
		return make_uniq<NumericPrefixRangeFilter<uint8_t>>();
	case PhysicalType::UINT16:
		return make_uniq<NumericPrefixRangeFilter<uint16_t>>();
	case PhysicalType::UINT32:
		return make_uniq<NumericPrefixRangeFilter<uint32_t>>();
	case PhysicalType::UINT64:
		return make_uniq<NumericPrefixRangeFilter<uint64_t>>();
	case PhysicalType::UINT128:
		return make_uniq<NumericPrefixRangeFilter<uhugeint_t>>();
	case PhysicalType::INT8:
		return make_uniq<NumericPrefixRangeFilter<int8_t>>();
	case PhysicalType::INT16:
		return make_uniq<NumericPrefixRangeFilter<int16_t>>();
	case PhysicalType::INT32:
		return make_uniq<NumericPrefixRangeFilter<int32_t>>();
	case PhysicalType::INT64:
		return make_uniq<NumericPrefixRangeFilter<int64_t>>();
	case PhysicalType::INT128:
		return make_uniq<NumericPrefixRangeFilter<hugeint_t>>();
	default:
		throw NotImplementedException("Prefix range filter is not implemented for type %s", type.ToString());
	}
}

bool PrefixRangeTableFilter::SupportedType(const LogicalType &type) {
	switch (type.InternalType()) {
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::UINT128:
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::INT128:
		return true;
	default:
		return false;
	}
}

PrefixRangeTableFilter::PrefixRangeTableFilter(unique_ptr<PrefixRangeFilter> filter_p, const bool filters_null_values_p,
                                               const string &key_column_name_p, const LogicalType &key_type_p)
    : TableFilter(TYPE), filter(std::move(filter_p)), filters_null_values(filters_null_values_p),
      key_column_name(key_column_name_p), key_type(key_type_p) {
}
string PrefixRangeTableFilter::ToString(const string &column_name) const {
	return column_name + " IN RF(" + key_column_name + ")";
}

idx_t PrefixRangeTableFilter::Filter(Vector &keys, SelectionVector &sel, idx_t &approved_tuple_count,
                                     PrefixRangeTableFilterState &state) const {
	if (state.current_capacity < approved_tuple_count) {
		state.sel.Initialize(approved_tuple_count);
		state.current_capacity = approved_tuple_count;
	}

	idx_t found_count;
	if (keys.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		const bool found = filter->LookupOne(keys.GetValue(0));
		found_count = found ? approved_tuple_count : 0;
	} else {
		keys.Flatten(approved_tuple_count);
		found_count = filter->LookupKeys(keys, state.sel, approved_tuple_count);
	}

	// all the elements have been found, we don't need to translate anything
	if (found_count == approved_tuple_count) {
		return approved_tuple_count;
	}

	if (sel.IsSet()) {
		for (idx_t idx = 0; idx < found_count; idx++) {
			const idx_t flat_sel_idx = state.sel.get_index(idx);
			const idx_t original_sel_idx = sel.get_index(flat_sel_idx);
			sel.set_index(idx, original_sel_idx);
		}
	} else {
		sel.Initialize(state.sel);
	}

	approved_tuple_count = found_count;
	return approved_tuple_count;
}

bool PrefixRangeTableFilter::FilterValue(const Value &value) const {
	return filter->LookupOne(value);
}

FilterPropagateResult PrefixRangeTableFilter::CheckStatistics(BaseStatistics &stats) const {
	if (!NumericStats::HasMinMax(stats)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}

	const auto min = NumericStats::Min(stats);
	const auto max = NumericStats::Max(stats);
	if (min > max) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}

	auto result_count = filter->LookupRange(min, max);
	if (result_count == 0) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	// TODO: If result_count == max - min => FILTER_ALWAYS_TRUE
	return FilterPropagateResult::NO_PRUNING_POSSIBLE;
}

bool PrefixRangeTableFilter::Equals(const TableFilter &other) const {
	if (!TableFilter::Equals(other)) {
		return false;
	}
	return false;
}

unique_ptr<TableFilter> PrefixRangeTableFilter::Copy() const {
	return make_uniq<PrefixRangeTableFilter>(this->filter->Copy(), this->filters_null_values, this->key_column_name,
	                                         this->key_type);
}

unique_ptr<Expression> PrefixRangeTableFilter::ToExpression(const Expression &column) const {
	// TODO: Do we have to do this as well?
	auto bound_constant = make_uniq<BoundConstantExpression>(Value(true));
	return std::move(bound_constant);
}

void PrefixRangeTableFilter::Serialize(Serializer &serializer) const {
	TableFilter::Serialize(serializer);
	serializer.WriteProperty<bool>(200, "filters_null_values", filters_null_values);
	serializer.WriteProperty<string>(201, "key_column_name", key_column_name);
	serializer.WriteProperty<LogicalType>(202, "key_type", key_type);
}

unique_ptr<TableFilter> PrefixRangeTableFilter::Deserialize(Deserializer &deserializer) {
	auto filters_null_values = deserializer.ReadProperty<bool>(200, "filters_null_values");
	auto key_column_name = deserializer.ReadProperty<string>(201, "key_column_name");
	auto key_type = deserializer.ReadProperty<LogicalType>(202, "key_type");

	auto result = make_uniq<PrefixRangeTableFilter>(CreateFilter(key_type), filters_null_values, key_column_name,
	                                                key_type);
	return std::move(result);
}
} // namespace duckdb
