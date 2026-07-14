#pragma once

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

	void Build(const std::vector<uint64_t> &keys, double bits_per_key);
	bool Probe(uint64_t key) const;
	bool RangeProbe(uint64_t lower, uint64_t upper) const;
	size_t SizeBytes() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace duckdb
