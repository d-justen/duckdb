#pragma once

#include "filter_benchmark_common.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace duckdb {

class DivaBenchmarkFilter {
public:
	DivaBenchmarkFilter();
	~DivaBenchmarkFilter();

	DivaBenchmarkFilter(const DivaBenchmarkFilter &) = delete;
	DivaBenchmarkFilter &operator=(const DivaBenchmarkFilter &) = delete;

	void Build(const std::vector<uint64_t> &sorted_keys, double bits_per_key, uint32_t seed = 42);
	bool Probe(uint64_t key) const;
	bool RangeProbe(uint64_t lower, uint64_t upper) const;
	size_t SizeBytes() const;
	BenchmarkBuildInfo BuildInfo() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace duckdb
