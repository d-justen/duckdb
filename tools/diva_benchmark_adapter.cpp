#include "diva_benchmark_adapter.hpp"

#include "diva.hpp"

#include <cmath>
#include <stdexcept>

namespace duckdb {

struct DivaBenchmarkFilter::Impl {
	std::unique_ptr<Diva<true>> filter;
	BenchmarkBuildInfo info;
};

DivaBenchmarkFilter::DivaBenchmarkFilter() = default;

DivaBenchmarkFilter::~DivaBenchmarkFilter() = default;

void DivaBenchmarkFilter::Build(const std::vector<uint64_t> &sorted_keys, double bits_per_key, uint32_t seed) {
	if (sorted_keys.empty() || !std::isfinite(bits_per_key) || bits_per_key <= 1) {
		throw std::invalid_argument("Diva requires nonempty sorted keys and finite bpk > 1");
	}
	if (!impl) {
		impl = std::make_unique<Impl>();
	}
	impl->info.representation = "diva";
	impl->info.requested_bpk = bits_per_key;
	static constexpr double LOAD_FACTOR = 0.95;

	const auto infix_size = static_cast<uint32_t>(std::lround(LOAD_FACTOR * (bits_per_key - 1.0)));
	impl->filter = std::make_unique<Diva<true>>(infix_size, sorted_keys.begin(), sorted_keys.end(), sizeof(uint64_t),
	                                            seed, LOAD_FACTOR);
}

bool DivaBenchmarkFilter::Probe(uint64_t key) const {
	return impl->filter->PointQuery(key);
}

bool DivaBenchmarkFilter::RangeProbe(uint64_t lower, uint64_t upper) const {
	return impl->filter->RangeQuery(lower, upper);
}

size_t DivaBenchmarkFilter::SizeBytes() const {
	// Count query state, excluding construction/measurement metadata.
	return sizeof(*this) + sizeof(impl->filter) + impl->filter->Size();
}

BenchmarkBuildInfo DivaBenchmarkFilter::BuildInfo() const {
	return impl->info;
}

} // namespace duckdb
