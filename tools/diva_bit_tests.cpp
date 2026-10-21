#include "util.hpp"
#include <iostream>
#include <random>
#include <stdexcept>

static void Check(uint64_t word) {
	uint32_t rank = 0;
	for (uint32_t bit = 0; bit < 64; ++bit) {
		if (word & (uint64_t(1) << bit)) {
			if (bit_select(word, rank++) != bit) {
				throw std::runtime_error("Diva select differs from reference");
			}
		}
	}
	if (rank < 64 && bit_select(word, rank) != 64) {
		throw std::runtime_error("Diva select missing-rank sentinel differs");
	}
	int32_t low = 64, high = -1;
	for (int32_t bit = 0; bit < 64; ++bit) {
		if (word & (uint64_t(1) << bit)) {
			low = std::min(low, bit);
			high = bit;
		}
	}
	if (lowbit_pos(word) != low || highbit_pos(word) != high || diva_clz64(word) != uint32_t(63 - high)) {
		throw std::runtime_error("Diva bit positions differ from reference");
	}
}

int main() {
	try {
		Check(0);
		Check(~uint64_t(0));
		for (uint32_t bit = 0; bit < 64; ++bit) {
			Check(uint64_t(1) << bit);
			Check(~(uint64_t(1) << bit));
		}
		std::mt19937_64 random(42);
		for (unsigned i = 0; i < 10000; ++i) {
			Check(random());
		}
		std::cout << "diva_bit_tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
