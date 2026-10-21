//===----------------------------------------------------------------------===//
// Internal, bounded scratch representation used only while compressing PRFs.
// The installed filter always uses the existing bitmap or direct-range layout.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/assert.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/vector.hpp"

#include <algorithm>

namespace duckdb {

struct PrefixRangeInterval {
	idx_t lower;
	idx_t upper;
};

class PrefixRangeIntervals {
public:
	static constexpr idx_t MAX_INTERVALS = 65536;
	static constexpr idx_t BLOCK_WORDS = 128;

	static bool Append(vector<PrefixRangeInterval> &result, idx_t lower, idx_t upper, idx_t limit) {
		if (!result.empty() && lower <= result.back().upper + 1) {
			result.back().upper = MaxValue(result.back().upper, upper);
			return true;
		}
		if (result.size() == limit) {
			return false;
		}
		if (result.size() == result.capacity()) {
			result.reserve(MinValue(limit, MaxValue<idx_t>(1024, result.capacity() * 2)));
		}
		result.push_back({lower, upper});
		return true;
	}

	// Sampling chooses an exact collection algorithm, never a compression decision.
	static bool PreferBlocked(const uint64_t *bitmap, idx_t word_count) {
		const auto blocks = word_count / BLOCK_WORDS;
		if (blocks < 16) {
			return true;
		}
		idx_t mixed = 0;
		for (idx_t sample = 0; sample < 16; sample++) {
			const auto begin = ((sample * 2654435761ULL + 1013904223ULL) % blocks) * BLOCK_WORDS;
			uint64_t any_bits = 0;
			uint64_t all_bits = ~uint64_t(0);
			for (idx_t i = begin; i < begin + BLOCK_WORDS; i++) {
				any_bits |= bitmap[i];
				all_bits &= bitmap[i];
			}
			mixed += any_bits != 0 && all_bits != ~uint64_t(0);
			if (mixed >= 4) {
				return false;
			}
		}
		return true;
	}

	static uint64_t Mask(idx_t bits) {
		return bits == 64 ? ~uint64_t(0) : (uint64_t(1) << bits) - 1;
	}

	// Failure leaves the source untouched. Parallel collectors divide the global
	// interval budget; an uneven distribution can safely fall back to bitmap reduction.
	template <bool BLOCKED>
	static bool Collect(const uint64_t *bitmap, idx_t logical_buckets, idx_t begin_word, idx_t end_word, idx_t limit,
	                    vector<PrefixRangeInterval> &result) {
		D_ASSERT(result.empty());
		for (idx_t block = begin_word; block < end_word; block += BLOCK_WORDS) {
			const auto end = MinValue(block + BLOCK_WORDS, end_word);
			if (BLOCKED) {
				uint64_t any_bits = 0;
				uint64_t all_bits = ~uint64_t(0);
				for (idx_t i = block; i < end; i++) {
					any_bits |= bitmap[i];
					all_bits &= bitmap[i];
				}
				if (any_bits == 0) {
					continue;
				}
				if (all_bits == ~uint64_t(0) && end <= logical_buckets / 64) {
					if (!Append(result, block * 64, end * 64 - 1, limit)) {
						return false;
					}
					continue;
				}
			}
			for (idx_t i = block; i < end; i++) {
				const auto base = i * 64;
				auto word = bitmap[i] & Mask(MinValue<idx_t>(64, logical_buckets - base));
				while (word) {
					const idx_t start = __builtin_ctzll(word);
					const auto remaining = word >> start;
					const idx_t length = remaining == ~uint64_t(0) ? 64 : __builtin_ctzll(~remaining);
					if (!Append(result, base + start, base + start + length - 1, limit)) {
						return false;
					}
					word &= ~(Mask(length) << start);
				}
			}
		}
		return true;
	}

	static idx_t Reduce(const vector<PrefixRangeInterval> &source, vector<PrefixRangeInterval> &destination) {
		destination.clear();
		destination.reserve(source.size());
		idx_t active = 0;
		for (const auto &interval : source) {
			const auto lower = interval.lower >> 1;
			const auto upper = interval.upper >> 1;
			if (!destination.empty() && lower <= destination.back().upper + 1) {
				active += upper - destination.back().upper;
				destination.back().upper = upper;
			} else {
				destination.push_back({lower, upper});
				active += upper - lower + 1;
			}
		}
		return active;
	}

	// Word-aligned partitions can be materialized concurrently without shared writes.
	static void Materialize(const vector<PrefixRangeInterval> &intervals, uint64_t *destination, idx_t begin_word,
	                        idx_t end_word) {
		D_ASSERT(begin_word < end_word);
		std::fill(destination + begin_word, destination + end_word, uint64_t(0));
		const auto begin_bit = begin_word * 64;
		const auto end_bit = end_word * 64 - 1;
		auto entry =
		    std::lower_bound(intervals.begin(), intervals.end(), begin_bit,
		                     [](const PrefixRangeInterval &interval, idx_t bit) { return interval.upper < bit; });
		for (; entry != intervals.end() && entry->lower <= end_bit; ++entry) {
			const auto lower = MaxValue(begin_bit, entry->lower);
			const auto upper = MinValue(end_bit, entry->upper);
			const auto first = lower / 64;
			const auto last = upper / 64;
			const auto left_mask = ~uint64_t(0) << (lower & 63);
			const auto right_mask = Mask((upper & 63) + 1);
			if (first == last) {
				destination[first] |= left_mask & right_mask;
			} else {
				destination[first] |= left_mask;
				std::fill(destination + first + 1, destination + last, ~uint64_t(0));
				destination[last] |= right_mask;
			}
		}
	}
};

} // namespace duckdb
