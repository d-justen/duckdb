//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/filter/prefix_range_filter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/filter_propagate_result.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace duckdb {

class ClientContext;
class Vector;
struct SelectionVector;

//! Runtime prefix-range filter state used by join pushdown and internal tablefilter functions.
class PrefixRangeFilter {
public:
	struct Sizing {
		uhugeint_t span;
		idx_t shift = 0;
	};

	struct Analysis {
		idx_t active_buckets = 0;
		double false_positive_rate = 0;
	};

	enum class CompressionMode : uint8_t { BITMAP, DIRECT_RANGES };

	struct CompressionInfo {
		CompressionMode mode = CompressionMode::BITMAP;
		idx_t shift = 0;
		idx_t range_count = 0;
		idx_t active_buckets = 0;
		idx_t run_count = 0;
		idx_t logical_bucket_count = 0;
		idx_t bitmap_allocation_bytes = 0;
		double false_positive_rate = 0;
	};

	struct BuildState {
		virtual ~BuildState() = default;
		template <class TARGET>

		TARGET &Cast() {
			DynamicCastCheck<TARGET>(this);
			return reinterpret_cast<TARGET &>(*this);
		}
		template <class TARGET>
		const TARGET &Cast() const {
			DynamicCastCheck<TARGET>(this);
			return reinterpret_cast<const TARGET &>(*this);
		}
	};

	virtual ~PrefixRangeFilter() = default;
	virtual void Initialize(ClientContext &context, idx_t number_of_rows, Value min, Value max,
	                        const Sizing &sizing) = 0;
	virtual unique_ptr<BuildState> InitializeBuildState(ClientContext &context) const = 0;
	virtual void InsertKeys(Vector &keys, BuildState &state) const = 0;
	virtual void InsertKeysParallel(Vector &keys, BuildState &state) const = 0;
	virtual void MergeBuildState(BuildState &state) = 0;
	virtual void Finalize() = 0;
	virtual idx_t GetBuildStateSize() const = 0;
	virtual idx_t LookupKeys(Vector &keys, SelectionVector &result_sel, idx_t count) const = 0;
	//! result_sel contains local positions into sel rather than source row ids.
	virtual idx_t LookupKeys(Vector &keys, const SelectionVector &sel, SelectionVector &result_sel,
	                         idx_t count) const = 0;
	virtual FilterPropagateResult LookupRange(const Value &lower_bound, const Value &upper_bound) const = 0;
	virtual bool IsInitialized() const = 0;
	virtual Analysis Analyze() const = 0;
	virtual CompressionInfo GetCompressionInfo() const = 0;
	static bool SupportedType(const LogicalType &type);
	static unique_ptr<PrefixRangeFilter> CreatePrefixRangeFilter(const LogicalType &key_type);
	static bool TryComputeSpan(const Value &lower_bound, const Value &upper_bound, uhugeint_t &result);
	static bool TryComputeSizing(const Value &min, const Value &max, idx_t count, Sizing &sizing,
	                             double false_positive_rate = 0.001);
	static bool TryComputeFixedSizeSizing(const Value &min, const Value &max, idx_t bucket_count_limit, Sizing &sizing);
	static bool TryComputeBucketCount(const uhugeint_t &span, idx_t shift, idx_t &bucket_count);
	static double ComputeFalsePositiveRateUpperBound(const uhugeint_t &span, idx_t count, idx_t shift);
	static double EstimateFalsePositiveRate(const uhugeint_t &span, idx_t positive_lower_bound, idx_t active_buckets,
	                                        idx_t shift);
};

//! DEPRECATED - only preserved for backwards-compatible expression conversion
class LegacyPrefixRangeTableFilter final : public TableFilter {
private:
	optional_ptr<PrefixRangeFilter> filter;

	string key_column_name;
	LogicalType key_type;

public:
	static constexpr auto TYPE = TableFilterType::LEGACY_PREFIX_RANGE_FILTER;

public:
	explicit LegacyPrefixRangeTableFilter(optional_ptr<PrefixRangeFilter> filter_p, const string &key_column_name_p,
	                                      const LogicalType &key_type_p);

private:
	unique_ptr<Expression> ToExpression(const Expression &column) const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<TableFilter> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
