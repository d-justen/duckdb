#pragma once

#include "filter_benchmark_common.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace duckdb {

class GrafiteBenchmarkFilter {
public:
	GrafiteBenchmarkFilter();
	~GrafiteBenchmarkFilter();

	GrafiteBenchmarkFilter(const GrafiteBenchmarkFilter &) = delete;
	GrafiteBenchmarkFilter &operator=(const GrafiteBenchmarkFilter &) = delete;

	void Build(const std::vector<uint64_t> &keys, double bits_per_key, uint32_t seed = 42);
	bool Probe(uint64_t key) const;
	bool RangeProbe(uint64_t lower, uint64_t upper) const;
	size_t SizeBytes() const;
	BenchmarkBuildInfo BuildInfo() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace duckdb
