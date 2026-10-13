#pragma once

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace duckdb {

// Every benchmark supplies sorted DISTINCT build keys. Input preparation and this
// validation are untimed; each summary's allocation, transformation and encoding
// belong inside its construction timer. Probe order is a separate property.
inline void ValidateBenchmarkBuildKeys(const std::vector<uint64_t> &keys) {
	if (keys.empty() ||
	    std::adjacent_find(keys.begin(), keys.end(), [](uint64_t a, uint64_t b) { return a >= b; }) != keys.end()) {
		throw std::invalid_argument("benchmark build keys must be nonempty, sorted and distinct");
	}
}

inline uint64_t BenchmarkBuildChecksum(const std::vector<uint64_t> &keys) {
	uint64_t hash = 14695981039346656037ULL;
	for (auto key : keys) {
		hash = (hash ^ key) * 1099511628211ULL;
	}
	return hash;
}

// Version 1: SplitMix64 finalizer, then low 32 bits (also accepted by Diva).
// No probe layout, execution order, or warmup count enters this seed.
inline uint32_t BenchmarkBuildSeed(uint64_t base, uint64_t checksum, uint64_t repetition) {
	uint64_t x = base ^ checksum ^ ((repetition + 1) * 0x9e3779b97f4a7c15ULL);
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
	return static_cast<uint32_t>(x ^ (x >> 31));
}

struct BenchmarkBuildInfo {
	const char *representation = "native";
	double requested_bpk = 0;
	uint64_t hash_a = 0, hash_b = 0, reduced_universe = 0;
	std::string CSV() const {
		std::ostringstream out;
		out << representation << ',' << std::setprecision(17) << requested_bpk << ',' << hash_a << ',' << hash_b << ','
		    << reduced_universe;
		return out.str();
	}
	static const char *Header() {
		return "adapter_representation,requested_bpk,hash_a,hash_b,reduced_universe";
	}
};

} // namespace duckdb
