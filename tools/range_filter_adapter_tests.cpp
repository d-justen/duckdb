#include "filter_benchmark_common.hpp"
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE)
#include "grafite_benchmark_adapter.hpp"
#endif
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_DIVA)
#include "diva_benchmark_adapter.hpp"
#endif
#include <iostream>
#include <limits>
#include <random>

using namespace duckdb;

static void Require(bool value, const char *message) {
	if (!value) {
		throw std::runtime_error(message);
	}
}

template <class Filter>
static void CheckQueries(const std::vector<uint64_t> &keys, double bpk, uint32_t seed, bool exact) {
	ValidateBenchmarkBuildKeys(keys);
	const auto original = keys;
	Filter first, replay;
	first.Build(keys, bpk, seed);
	replay.Build(keys, bpk, seed);
	Require(keys == original, "adapter modified shared build input");
	Require(first.SizeBytes() == replay.SizeBytes(), "replayed size differs");
	Require(first.BuildInfo().CSV() == replay.BuildInfo().CSV(), "replayed construction differs");
	for (uint64_t key = 0; key <= 320; key++) {
		const bool expected = std::binary_search(keys.begin(), keys.end(), key);
		const bool found = first.Probe(key);
		Require(found == replay.Probe(key), "replayed point result differs");
		Require(!expected || found, "point false negative");
		Require(!exact || expected == found, "exact point false positive");
		for (uint64_t upper = key; upper <= 320; upper++) {
			const auto next = std::lower_bound(keys.begin(), keys.end(), key);
			const bool overlap = next != keys.end() && *next <= upper;
			const bool result = first.RangeProbe(key, upper);
			Require(!overlap || result, "range false negative");
			Require(!exact || overlap == result, "exact range false positive");
		}
	}
	Require(first.RangeProbe(0, std::numeric_limits<uint64_t>::max()), "full universe rejected");
}

int main() {
	try {
		Require(BenchmarkBuildSeed(42, 123, 0) == 1501354885U, "construction seed regression");
		for (const auto &bad : std::vector<std::vector<uint64_t>> {{}, {2, 1}, {1, 1}}) {
			bool rejected = false;
			try {
				ValidateBenchmarkBuildKeys(bad);
			} catch (const std::invalid_argument &) {
				rejected = true;
			}
			Require(rejected, "invalid shared input accepted");
		}
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE)
		const std::vector<std::vector<uint64_t>> cases {
		    {0}, {9}, {0, 1, 2, 3, 4, 5}, {0, 1, 10, 11, 20, 21}, {1, 63, 64, 127, 255, 300}};
		for (const auto &keys : cases) {
			for (uint32_t seed = 0; seed < 12; seed++) {
				CheckQueries<GrafiteBenchmarkFilter>(keys, 12, seed, true);
				CheckQueries<GrafiteBenchmarkFilter>(keys, 2.5, seed, false);
			}
		}
		GrafiteBenchmarkFilter filter;
		std::vector<uint64_t> sparse {0, 31, 80, 151, 255, 300};
		filter.Build(sparse, 2.5, 1);
		Require(std::string(filter.BuildInfo().representation) == "approximate_grafite", "approximate mode missing");
		const auto first_hash = filter.BuildInfo().CSV();
		filter.Build(sparse, 2.5, 2);
		Require(filter.BuildInfo().CSV() != first_hash, "seed does not change approximate construction");
		filter.Build(sparse, 12, 1);
		Require(std::string(filter.BuildInfo().representation) == "exact_elias_fano", "exact fallback missing");
		Require(filter.BuildInfo().requested_bpk == 12, "requested budget was clamped");
		const auto maximum = std::numeric_limits<uint64_t>::max();
		for (const auto &keys : std::vector<std::vector<uint64_t>> {{maximum}, {0, maximum}, {maximum - 5, maximum}}) {
			filter.Build(keys, 128, 42);
			for (auto key : keys) {
				Require(filter.Probe(key), "maximum endpoint rejected");
			}
			Require(!filter.RangeProbe(maximum - 4, maximum - 1), "exact upper gap accepted");
			Require(filter.RangeProbe(maximum, maximum), "maximum singleton rejected");
		}
		for (double bpk : {1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
			bool rejected = false;
			try {
				filter.Build(sparse, bpk, 42);
			} catch (const std::invalid_argument &) {
				rejected = true;
			}
			Require(rejected, "invalid budget accepted");
		}
#endif
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_DIVA)
		for (uint32_t seed : {1, 42, 1024}) {
			CheckQueries<DivaBenchmarkFilter>({0, 1, 2, 97, 98, 99, 255, 300}, 16, seed, false);
		}
		// Exercise multi-block infix stores and the top bit of uint64 keys.
		std::mt19937_64 random(42);
		for (uint64_t count : {1, 2, 1023, 1024, 1025, 2049}) {
			std::vector<uint64_t> keys;
			for (uint64_t i = 0; i < count; ++i) {
				keys.push_back(i * 1009);
			}
			DivaBenchmarkFilter filter;
			filter.Build(keys, 16, 42);
			for (auto key : keys) {
				Require(filter.Probe(key), "Diva boundary-size false negative");
			}
		}
		for (bool clustered : {false, true}) {
			std::vector<uint64_t> keys {0, std::numeric_limits<uint64_t>::max()};
			for (uint64_t i = 0; i < 65536; ++i) {
				keys.push_back(clustered ? (i / 64) * 1000003 + (i % 64) : random());
			}
			std::sort(keys.begin(), keys.end());
			keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
			DivaBenchmarkFilter filter;
			filter.Build(keys, 16, 42);
			for (auto key : keys) {
				Require(filter.Probe(key), "Diva point false negative");
				Require(filter.RangeProbe(key, key), "Diva singleton false negative");
			}
			for (unsigned i = 0; i < 2000; ++i) {
				auto lower = random(), upper = random();
				if (lower > upper) {
					std::swap(lower, upper);
				}
				auto found = std::lower_bound(keys.begin(), keys.end(), lower);
				Require(found == keys.end() || *found > upper || filter.RangeProbe(lower, upper),
				        "Diva range false negative");
			}
		}
#endif
		std::cout << "range_filter_adapter_tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
