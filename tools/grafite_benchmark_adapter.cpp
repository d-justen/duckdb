#include "grafite_benchmark_adapter.hpp"

#include "grafite/grafite.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace duckdb {

struct GrafiteBenchmarkFilter::Impl {
	grafite::filter<> filter;
};

GrafiteBenchmarkFilter::GrafiteBenchmarkFilter() : impl(std::make_unique<Impl>()) {
}

GrafiteBenchmarkFilter::~GrafiteBenchmarkFilter() = default;

static double ClampGrafiteBitsPerKey(const std::vector<uint64_t> &keys, double bits_per_key) {
	const auto max_key = *std::max_element(keys.begin(), keys.end());
	if (max_key == 0) {
		return bits_per_key;
	}
	const auto key_count = static_cast<double>(keys.size());
	const auto max_supported = 2.0 + std::log2(static_cast<double>(max_key) / key_count);
	return std::min(bits_per_key, std::nextafter(max_supported, 0.0));
}

void GrafiteBenchmarkFilter::Build(const std::vector<uint64_t> &keys, double bits_per_key) {
	if (keys.empty()) {
		throw std::invalid_argument("Grafite build keys must not be empty");
	}
	impl->filter = grafite::filter<>(keys.begin(), keys.end(), ClampGrafiteBitsPerKey(keys, bits_per_key));
}

bool GrafiteBenchmarkFilter::Probe(uint64_t key) const {
	return impl->filter.query(key);
}

size_t GrafiteBenchmarkFilter::SizeBytes() const {
	return impl->filter.size();
}

} // namespace duckdb
