#include "diva_benchmark_adapter.hpp"

#include "diva.hpp"

#include <cmath>
#include <stdexcept>

namespace duckdb {

struct DivaBenchmarkFilter::Impl {
	std::unique_ptr<Diva<true>> filter;
};

DivaBenchmarkFilter::DivaBenchmarkFilter() : impl(std::make_unique<Impl>()) {
}

DivaBenchmarkFilter::~DivaBenchmarkFilter() = default;

void DivaBenchmarkFilter::Build(const std::vector<uint64_t> &sorted_keys, double bits_per_key) {
	if (sorted_keys.empty()) {
		throw std::invalid_argument("Diva build keys must not be empty");
	}
	static constexpr uint32_t RNG_SEED = 1024;
	static constexpr double LOAD_FACTOR = 0.95;

	const auto infix_size = static_cast<uint32_t>(std::lround(LOAD_FACTOR * (bits_per_key - 1.0)));
	impl->filter = std::make_unique<Diva<true>>(infix_size, sorted_keys.begin(), sorted_keys.end(), sizeof(uint64_t),
	                                            RNG_SEED, LOAD_FACTOR);
}

bool DivaBenchmarkFilter::Probe(uint64_t key) const {
	return impl->filter->PointQuery(key);
}

bool DivaBenchmarkFilter::RangeProbe(uint64_t lower, uint64_t upper) const {
	return impl->filter->RangeQuery(lower, upper);
}

size_t DivaBenchmarkFilter::SizeBytes() const {
	return impl->filter->Size();
}

} // namespace duckdb
