/*
 * Sux: Succinct data structures
 *
 * Copyright (C) 2007-2020 Sebastiano Vigna
 *
 *  This library is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU Lesser General Public License as published by the Free
 *  Software Foundation; either version 3 of the License, or (at your option)
 *  any later version.
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *
 * Under Section 7 of GPL version 3, you are granted additional permissions
 * described in the GCC Runtime Library Exception, version 3.1, as published by
 * the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License and a copy of
 * the GCC Runtime Library Exception along with this program; see the files
 * COPYING3 and COPYING.RUNTIME respectively.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <sux/bits/Rank.hpp>
#include <sux/bits/SimpleSelectHalf.hpp>
#include <sux/bits/SimpleSelectZeroHalf.hpp>
#include <cstdint>
#include <vector>

namespace sux::bits {

using namespace std;
using namespace sux;

/** An implementation of selection and ranking based on the Elias-Fano representation
 * of monotone sequences.
 *
 * Instances of this class can be built using a bit vector or an explicit list of
 * positions for the ones in a vector. In every case, the bit vector or the list
 * are not necessary after construction.
 *
 * @tparam AT a type of memory allocation out of sux::util::AllocType.
 */

template <util::AllocType AT = util::AllocType::MALLOC, bool AllowRank = true> class EliasFano
{
public:
    util::Vector<uint64_t, AT> lower_bits, upper_bits;
    SimpleSelectZeroHalf<AT> selectz_upper;
    uint64_t num_bits, num_ones;
    int l;
    uint64_t lower_l_bits_mask;

    __inline static void set(util::Vector<uint64_t, AT> &bits, const uint64_t pos)
    { bits[pos / 64] |= 1ULL << pos % 64; }

    __inline static uint64_t get_bits(const util::Vector<uint64_t, AT> &bits, const uint64_t start, const int width)
    {
        const int start_word = start / 64;
        const int start_bit = start % 64;
        const int total_offset = start_bit + width;
        const uint64_t result = bits[start_word] >> start_bit;
        return (total_offset <= 64 ? result : result | bits[start_word + 1] << (64 - start_bit)) &
               ((1ULL << width) - 1);
    }

    __inline static void
    set_bits(util::Vector<uint64_t, AT> &bits, const uint64_t start, const int width, const uint64_t value)
    {
        const uint64_t start_word = start / 64;
        const uint64_t end_word = (start + width - 1) / 64;
        const uint64_t start_bit = start % 64;

        if (start_word == end_word)
        {
            bits[start_word] &= ~(((1ULL << width) - 1) << start_bit);
            bits[start_word] |= value << start_bit;
        }
        else
        {
            // Here start_bit > 0.
            bits[start_word] &= (1ULL << start_bit) - 1;
            bits[start_word] |= value << start_bit;
            bits[end_word] &= -(1ULL << (width - 64 + start_bit));
            bits[end_word] |= value >> (64 - start_bit);
        }
    }

public:

    EliasFano() = default;

    /** Creates a new instance using an
     *  explicit list of positions for the ones in a bit vector.
     *
     *  Note that the list is read only at construction time.
     *
     *  In practice this constructor builds an Elias-Fano
     *  representation of the given list. select(const uint64_t rank) will retrieve
     *  an element of the list, and rank(const size_t pos) will return how many
     *  element of the list are smaller than the argument.
     *
     * @param begin an iterator to the beginning of the list.
     * @param end an iterator to the end of the list.
     * @param remove_duplicates if true, duplicates in the list are removed. (NOTE: if true, the original list can be modified)
     */
    template <class t_itr>
    EliasFano(const t_itr begin, const t_itr end, bool remove_duplicates = false)
    {
        auto last = (remove_duplicates) ? std::unique(begin, end) : end;

        num_ones = std::distance(begin, last);
        this->num_bits = *(last - 1) + 1;
        l = num_ones == 0 ? 0 : max(0, lambda_safe(num_bits / num_ones));

#ifdef DEBUG
        printf("Number of ones: %lld l: %d\n", num_ones, l);
        printf("Upper bits: %lld\n", num_ones + (num_bits >> l) + 1);
        printf("Lower bits: %lld\n", num_ones * l);
#endif

        const uint64_t lower_bits_mask = (1ULL << l) - 1;

        lower_bits.size((num_ones * l + 63) / 64 + 2 * (l == 0));
        upper_bits.size(((num_ones + (num_bits >> l) + 1) + 63) / 64);

        size_t i = 0;
        for (auto it = begin; it < last; ++it)
        {
            if (l != 0) set_bits(lower_bits, i * l, l, *it & lower_bits_mask);
            set(upper_bits, (*it >> l) + i);
            ++i;
        }

#ifdef DEBUG
        printf("First lower: %016llx %016llx %016llx %016llx\n", lower_bits[0], lower_bits[1], lower_bits[2], lower_bits[3]);
        printf("First upper: %016llx %016llx %016llx %016llx\n", upper_bits[0], upper_bits[1], upper_bits[2], upper_bits[3]);
#endif

        if constexpr (AllowRank)
            selectz_upper = SimpleSelectZeroHalf(&upper_bits, num_ones + (num_bits >> l));

        lower_l_bits_mask = (1ULL << l) - 1;
    }

    uint64_t rank(const size_t k) const
    {
        static_assert(AllowRank, "Cannot call rank() if AllowRank is false");

        if (num_ones == 0) return 0;
        if (k >= num_bits) return num_ones;
#ifdef DEBUG
        printf("Ranking %lld...\n", k);
#endif
        const uint64_t k_shiftr_l = k >> l;


        int64_t pos = selectz_upper.selectZero(k_shiftr_l);
        uint64_t rank = pos - (k_shiftr_l);

#ifdef DEBUG
        printf("Position: %lld rank: %lld\n", pos, rank);
#endif
        uint64_t rank_times_l = rank * l;
        const uint64_t k_lower_bits = k & lower_l_bits_mask;

        do
        {
            rank--;
            rank_times_l -= l;
            pos--;
        } while (pos >= 0 && (upper_bits[pos / 64] & 1ULL << pos % 64) &&
                 get_bits(lower_bits, rank_times_l, l) >= k_lower_bits);

        return ++rank;
    }

    uint64_t rankv2(const size_t k) const
    {
        if (num_ones == 0) return 0;
        if (k >= num_bits) return num_ones;

        const uint64_t k_shiftr_l = k >> l;

        uint64_t pos_hi = selectz_upper.selectZero(k_shiftr_l);;
        uint64_t pos_lo = 0;
        if (k_shiftr_l != 0)
            pos_lo = selectz_upper.selectZero(k_shiftr_l - 1) + 1;

        size_t pos;
        size_t rank;
        auto count = pos_hi - pos_lo;

        if (count < 8) {
            pos = pos_hi;
            rank = pos_hi - k_shiftr_l;
            uint64_t rank_times_l = rank * l;
            const uint64_t k_lower_bits = k & lower_l_bits_mask;
            do
            {
                rank--;
                rank_times_l -= l;
                pos--;
            } while (pos >= pos_lo && (upper_bits[pos / 64] & 1ULL << pos % 64) &&
                     get_bits(lower_bits, rank_times_l, l) >= k_lower_bits);
        } else {
            auto rank_lo = pos_lo - k_shiftr_l;
            auto rank_hi = pos_hi - k_shiftr_l;
            const uint64_t k_lower_bits = k & lower_l_bits_mask;

            while (count > 0)
            {
                auto step = count / 2;
                auto mid = rank_lo + step;
                if (get_bits(lower_bits, mid * l, l) < k_lower_bits)
                {
                    rank_lo = mid + 1;
                    count -= step + 1;
                }
                else
                {
                    count = step;
                }
            }
            --rank_lo;
            pos = pos_hi - (rank_hi - rank_lo);
            rank = rank_lo;
        }
        return ++rank;
    }

    struct ElementPointer {
        size_t rank;
        size_t pos_upper;
        const EliasFano<AT, AllowRank> *ef;

        ElementPointer(size_t rank, size_t pos_upper, const EliasFano<AT, AllowRank> *ef)
            : rank(rank), pos_upper(pos_upper), ef(ef) {}


        uint64_t operator*() const {
            return (pos_upper - rank) << ef->l | get_bits(ef->lower_bits, rank * ef->l, ef->l);
        }

        size_t index() const { return rank; }

        ElementPointer& operator++() {
            rank++;
            auto curr = pos_upper / 64;
            uint64_t window = ef->upper_bits[curr] & -1ULL << pos_upper;
            window &= window - 1;
            while (window == 0) window = ef->upper_bits[++curr];
            pos_upper = curr * 64 + __builtin_ctzll(window);
            return *this;
        }
    };

    ElementPointer at(size_t rank) const {
        return ElementPointer(rank, 0, this);
    }


    ElementPointer predecessor(const size_t k) const {
        static_assert(AllowRank, "Cannot call predecessor() if AllowRank is false");
        const uint64_t k_shiftr_l = k >> l;

        uint64_t pos_hi;
        uint64_t pos_lo = 0;
        if (k_shiftr_l == 0) {
            pos_hi = selectz_upper.selectZero(k_shiftr_l);
        } else {
            pos_lo = selectz_upper.selectZero(k_shiftr_l - 1, &pos_hi) + 1;
        }

        int64_t pos;
        size_t rank;
        auto count = pos_hi - pos_lo;

        if (count < 8) {
            pos = pos_hi;
            rank = pos_hi - k_shiftr_l;
            uint64_t rank_times_l = rank * l;
            const uint64_t k_lower_bits = k & lower_l_bits_mask;
            do {
                rank--;
                rank_times_l -= l;
                pos--;
            } while (pos >= pos_lo && get_bits(lower_bits, rank_times_l, l) > k_lower_bits);
        } else {
            auto rank_lo = pos_lo - k_shiftr_l;
            auto rank_hi = pos_hi - k_shiftr_l;
            const uint64_t k_lower_bits = k & lower_l_bits_mask;

            while (count > 0) {
                auto step = count / 2;
                auto mid = rank_lo + step;
                if (get_bits(lower_bits, mid * l, l) <= k_lower_bits) {
                    rank_lo = mid + 1;
                    count -= step + 1;
                } else {
                    count = step;
                }
            }
            --rank_lo;
            pos = pos_hi - (rank_hi - rank_lo);
            rank = rank_lo;
        }

        if (pos > 0 && (upper_bits[pos / 64] & 1ULL << pos % 64) == 0) {
            // find previous set bit
            auto curr = pos / 64;
            uint64_t word = upper_bits[curr] & ((1ULL << pos % 64) - 1);
            while (word == 0) word = upper_bits[--curr];
            pos = curr * 64 + 63 - __builtin_clzll(word);
        }

        return ElementPointer(rank, pos, this);
    }

    size_t numOnes() const { return num_ones; }

    /** Returns an estimate of the size in bits of this structure. */
    uint64_t bitCount() const {
        auto select_upper = SimpleSelectHalf(&upper_bits, num_ones + (num_bits >> l));
        return upper_bits.bitCount() - sizeof(upper_bits) * 8 + lower_bits.bitCount() - sizeof(lower_bits) * 8 + select_upper.bitCount() - sizeof(select_upper) * 8 + selectz_upper.bitCount() -
            sizeof(selectz_upper) * 8 + sizeof(*this) * 8;
    }

    friend std::ostream& operator<<(std::ostream& out, const EliasFano& ef) {
        out.write(reinterpret_cast<const char*>(&ef.num_bits), sizeof(ef.num_bits));
        out.write(reinterpret_cast<const char*>(&ef.l), sizeof(ef.l));
        out.write(reinterpret_cast<const char*>(&ef.num_ones), sizeof(ef.num_ones));
        out.write(reinterpret_cast<const char*>(&ef.lower_l_bits_mask), sizeof(ef.lower_l_bits_mask));
        out << ef.selectz_upper;
        out << ef.upper_bits;
        out << ef.lower_bits;
        return out;
    }

    friend std::istream& operator>>(std::istream& in, EliasFano& ef) {
        in.read(reinterpret_cast<char*>(&ef.num_bits), sizeof(ef.num_bits));
        in.read(reinterpret_cast<char*>(&ef.l), sizeof(ef.l));
        in.read(reinterpret_cast<char*>(&ef.num_ones), sizeof(ef.num_ones));
        in.read(reinterpret_cast<char*>(&ef.lower_l_bits_mask), sizeof(ef.lower_l_bits_mask));
        in >> ef.selectz_upper;
        in >> ef.upper_bits;
        in >> ef.lower_bits;
        return in;
    }
};

} // namespace sux::bits
