#include "catch.hpp"

#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

static BaseStatistics DeserializeLegacyUnknownStringStats() {
	Allocator allocator;
	MemoryStream stream(allocator);
	BinarySerializer serializer(stream);
	serializer.Begin();
	data_t min[StringStatsData::MAX_STRING_MINMAX_SIZE];
	data_t max[StringStatsData::MAX_STRING_MINMAX_SIZE];
	for (idx_t i = 0; i < StringStatsData::MAX_STRING_MINMAX_SIZE; i++) {
		min[i] = 0;
		max[i] = 0xFF;
	}
	serializer.WriteProperty(200, "min", min, StringStatsData::MAX_STRING_MINMAX_SIZE);
	serializer.WriteProperty(201, "max", max, StringStatsData::MAX_STRING_MINMAX_SIZE);
	serializer.WriteProperty(202, "has_unicode", true);
	serializer.WriteProperty(203, "has_max_string_length", false);
	serializer.WriteProperty(204, "max_string_length", uint32_t(0));
	serializer.End();

	stream.Rewind();
	BinaryDeserializer deserializer(stream);
	auto result = StringStats::CreateUnknown(LogicalType::VARCHAR);
	deserializer.Begin();
	StringStats::Deserialize(deserializer, result);
	deserializer.End();
	return result;
}

TEST_CASE("StringStats HasMinMax handles unknown and legacy unknown stats", "[serialization]") {
	auto empty = StringStats::CreateEmpty(LogicalType::VARCHAR);
	REQUIRE(!StringStats::HasMinMax(empty));

	auto unknown = StringStats::CreateUnknown(LogicalType::VARCHAR);
	REQUIRE(!StringStats::HasMinMax(unknown));

	StringStats::Update(empty, string_t("hello"));
	REQUIRE(StringStats::HasMinMax(empty));
	REQUIRE(StringStats::Min(empty) == "hello");
	REQUIRE(StringStats::Max(empty) == "hello");

	auto legacy_unknown = DeserializeLegacyUnknownStringStats();
	REQUIRE(!StringStats::HasMinMax(legacy_unknown));
	REQUIRE(StringStats::Min(legacy_unknown) > StringStats::Max(legacy_unknown));
}

} // namespace duckdb
