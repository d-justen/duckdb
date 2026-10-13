#include "grafite_benchmark_adapter.hpp"
#include "grafite/grafite.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace duckdb {

struct GrafiteBenchmarkFilter::Impl {
	std::unique_ptr<grafite::filter<>> approximate;
	std::unique_ptr<grafite::ef_sux_vector> exact;
	uint64_t first = 0, last = 0;
	BenchmarkBuildInfo info;
};

GrafiteBenchmarkFilter::GrafiteBenchmarkFilter() = default;
GrafiteBenchmarkFilter::~GrafiteBenchmarkFilter() = default;

void GrafiteBenchmarkFilter::Build(const std::vector<uint64_t> &keys, double bits_per_key, uint32_t seed) {
	// Sorted distinct input is validated once by the harness, outside all timers.
	if (keys.empty() || !std::isfinite(bits_per_key) || bits_per_key < 2) {
		throw std::invalid_argument("Grafite requires nonempty sorted keys and finite bpk >= 2");
	}
	if (!impl) {
		impl = std::make_unique<Impl>();
	}
	impl->approximate.reset();
	impl->exact.reset();
	impl->first = keys.front();
	impl->last = keys.back();
	impl->info = BenchmarkBuildInfo();
	impl->info.requested_bpk = bits_per_key;
	const long double r = std::ceil(static_cast<long double>(keys.size()) * std::exp2(bits_per_key - 2));
	// The upstream constructor rejects budgets at which lossless EF is cheaper.
	// Store that exact representation instead of silently lowering the budget.
	if (r >= static_cast<long double>(keys.back())) {
		impl->info.representation = "exact_elias_fano";
		// SUX's constructor accepts mutable iterators even without deduplication.
		// This copy and all encoding work are part of the construction timer.
		auto input = keys;
		// SUX cannot encode UINT64_MAX (its universe is max+1). Keep that
		// endpoint in the common min/max metadata and query it directly.
		if (input.back() == std::numeric_limits<uint64_t>::max()) {
			input.pop_back();
		}
		if (!input.empty()) {
			impl->exact = std::make_unique<grafite::ef_sux_vector>(input.begin(), input.end(), false);
		}
	} else {
		impl->info.representation = "approximate_grafite";
		impl->approximate =
		    std::make_unique<grafite::filter<>>(keys.begin(), keys.end(), static_cast<uint64_t>(r), seed);
		impl->info.hash_a = impl->approximate->hash_a();
		impl->info.hash_b = impl->approximate->hash_b();
		impl->info.reduced_universe = impl->approximate->reduced_universe();
	}
}

bool GrafiteBenchmarkFilter::Probe(uint64_t key) const {
	if (key < impl->first || key > impl->last) {
		return false;
	}
	if (impl->approximate) {
		return impl->approximate->query(key);
	}
	return key == impl->last || impl->exact->check_presence(key);
}

bool GrafiteBenchmarkFilter::RangeProbe(uint64_t lower, uint64_t upper) const {
	if (lower > upper) {
		throw std::invalid_argument("Grafite range endpoints must be ordered");
	}
	if (upper < impl->first || lower > impl->last) {
		return false;
	}
	lower = std::max(lower, impl->first);
	upper = std::min(upper, impl->last);
	if (!impl->approximate) {
		return upper == impl->last || impl->exact->check_presence(lower, upper);
	}
	const auto r = impl->approximate->reduced_universe();
	// An interval covering a whole hash block maps onto the whole reduced
	// universe. Endpoints alone are insufficient for multi-block intervals.
	if (upper - lower >= r - 1) {
		return true;
	}
	if (lower / r == upper / r) {
		return impl->approximate->query(lower, upper);
	}
	const auto boundary = upper - upper % r;
	return impl->approximate->query(lower, boundary - 1) || impl->approximate->query(boundary, upper);
}

size_t GrafiteBenchmarkFilter::SizeBytes() const {
	// Count resident query state; exclude diagnostic metadata and construction scratch.
	return sizeof(*this) + 2 * sizeof(uint64_t) + sizeof(impl->approximate) + sizeof(impl->exact) +
	       (impl->approximate ? impl->approximate->size() : (impl->exact ? impl->exact->size() : 0));
}

BenchmarkBuildInfo GrafiteBenchmarkFilter::BuildInfo() const {
	return impl->info;
}

} // namespace duckdb
