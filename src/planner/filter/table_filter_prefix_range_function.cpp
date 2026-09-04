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
#include "duckdb/planner/table_filter_state.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

namespace duckdb {

PrefixRangeFunctionData::PrefixRangeFunctionData(optional_ptr<PrefixRangeFilter> filter_p, bool filters_null_values_p,
                                                 const string &key_column_name_p, const LogicalType &key_type_p,
                                                 float selectivity_threshold_p, idx_t n_vectors_to_check_p,
                                                 bool filters_tuples_p)
    : filter(filter_p), filters_null_values(filters_null_values_p), filters_tuples(filters_tuples_p),
      key_column_name(key_column_name_p), key_type(key_type_p), selectivity_threshold(selectivity_threshold_p),
      n_vectors_to_check(n_vectors_to_check_p) {
}

unique_ptr<FunctionData> PrefixRangeFunctionData::Copy() const {
	return make_uniq<PrefixRangeFunctionData>(filter, filters_null_values, key_column_name, key_type,
	                                          selectivity_threshold, n_vectors_to_check, filters_tuples);
}

bool PrefixRangeFunctionData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<PrefixRangeFunctionData>();
	return filter.get() == other.filter.get() && filters_null_values == other.filters_null_values &&
	       filters_tuples == other.filters_tuples && key_column_name == other.key_column_name &&
	       key_type == other.key_type;
}

static idx_t SelectPrefixRange(Vector &input, const PrefixRangeFunctionData &func_data, SelectionVector &result_sel,
                               idx_t count) {
	D_ASSERT(func_data.filter);
	const auto filter_count = func_data.filter->LookupKeys(input, result_sel, count);
	if (func_data.filters_null_values || filter_count == count) {
		return filter_count;
	}

	UnifiedVectorFormat input_data;
	input.ToUnifiedFormat(input_data);
	if (input_data.validity.CannotHaveNull()) {
		return filter_count;
	}

	SelectionVector filter_sel(filter_count);
	for (idx_t i = 0; i < filter_count; i++) {
		filter_sel.set_index(i, result_sel.get_index_unsafe(i));
	}
	idx_t result_count = 0;
	idx_t filter_idx = 0;
	for (idx_t i = 0; i < count; i++) {
		const auto matched = filter_idx < filter_count && filter_sel.get_index_unsafe(filter_idx) == i;
		filter_idx += matched;
		const auto input_idx = input_data.sel->get_index(i);
		if (matched || !input_data.validity.RowIsValid(input_idx)) {
			result_sel.set_index(result_count++, i);
		}
	}
	return result_count;
}

static unique_ptr<FunctionLocalState>
PrefixRangeInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr, FunctionData *bind_data) {
	auto &data = bind_data->Cast<PrefixRangeFunctionData>();
	if (!data.filter || !data.filters_tuples) {
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
	if (!func_data.filter || !func_data.filter->IsInitialized() || !func_data.filters_tuples) {
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
	if (!data.filters_null_values) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto column_stats = input.ChildStats(0);
	if (!column_stats) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &stats = *column_stats;
	switch (stats.GetStatsType()) {
	case StatisticsType::NUMERIC_STATS: {
		if (!NumericStats::HasMinMax(stats)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		const auto min = NumericStats::Min(stats);
		const auto max = NumericStats::Max(stats);
		if (min > max) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return data.filter->LookupRange(min, max);
	}
	case StatisticsType::STRING_STATS: {
		if (!StringStats::HasMinMax(stats)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		// String stats may contain raw parquet bytes that are not valid UTF-8. Reconstruct them as BLOBs so the
		// prefix-range comparable logic can inspect the raw bytes without value-construction validation.
		return data.filter->LookupRange(Value::BLOB_RAW(StringStats::Min(stats)),
		                                Value::BLOB_RAW(StringStats::Max(stats)));
	}
	default:
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
}

ScalarFunction TableFilterPrefixRangeFun::GetFunction() {
	return PrefixRangeScalarFun::GetFunction(LogicalType::ANY);
}

} // namespace duckdb
