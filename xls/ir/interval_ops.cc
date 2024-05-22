// Copyright 2023 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/ir/interval_ops.h"

#include <algorithm>
#include <compare>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/log/check.h"
#include "absl/types/span.h"
#include "xls/ir/bits.h"
#include "xls/ir/bits_ops.h"
#include "xls/ir/interval.h"
#include "xls/ir/interval_set.h"
#include "xls/ir/lsb_or_msb.h"
#include "xls/ir/node.h"
#include "xls/ir/ternary.h"
#include "xls/passes/ternary_evaluator.h"

namespace xls::interval_ops {

namespace {
TernaryVector ExtractTernaryInterval(const Interval& interval) {
  Bits lcp = bits_ops::LongestCommonPrefixMSB(
      {interval.LowerBound(), interval.UpperBound()});
  int64_t size = interval.BitCount();
  TernaryVector result(size, TernaryValue::kUnknown);
  for (int64_t i = size - lcp.bit_count(), j = 0; i < size; ++i, ++j) {
    result[i] = lcp.Get(j) ? TernaryValue::kKnownOne : TernaryValue::kKnownZero;
  }
  return result;
}
}  // namespace

TernaryVector ExtractTernaryVector(const IntervalSet& intervals,
                                   std::optional<Node*> source) {
  CHECK(intervals.IsNormalized())
      << (source.has_value() ? source.value()->ToString() : "");
  CHECK(!intervals.Intervals().empty())
      << (source.has_value() ? source.value()->ToString() : "");
  TernaryVector result = ExtractTernaryInterval(intervals.Intervals().front());
  for (const Interval& i : intervals.Intervals().subspan(1)) {
    TernaryVector t = ExtractTernaryInterval(i);
    ternary_ops::UpdateWithIntersection(result, t);
  }
  return result;
}

KnownBits ExtractKnownBits(const IntervalSet& intervals,
                           std::optional<Node*> source) {
  TernaryVector result = ExtractTernaryVector(intervals, source);
  return KnownBits{.known_bits = ternary_ops::ToKnownBits(result),
                   .known_bit_values = ternary_ops::ToKnownBitsValues(result)};
}

IntervalSet FromTernary(TernarySpan tern, int64_t max_interval_bits) {
  CHECK_GE(max_interval_bits, 0);
  if (ternary_ops::IsFullyKnown(tern)) {
    return IntervalSet::Precise(ternary_ops::ToKnownBitsValues(tern));
  }
  // How many trailing bits are unknown. This defines the size of each group.
  int64_t lsb_xs = absl::c_find_if(tern, ternary_ops::IsKnown) - tern.cbegin();
  // Find where we need to extend the unknown region to.
  std::deque<TernarySpan::const_iterator> x_locations;
  for (auto it = tern.cbegin() + lsb_xs; it != tern.cend(); ++it) {
    if (ternary_ops::IsUnknown(*it)) {
      x_locations.push_back(it);
      if (x_locations.size() > max_interval_bits + 1) {
        x_locations.pop_front();
      }
    }
  }
  if (x_locations.size() > max_interval_bits) {
    // Need to extend the x-s to avoid creating too many intervals.
    lsb_xs = (x_locations.front() - tern.cbegin()) + 1;
    x_locations.pop_front();
  }

  IntervalSet is(tern.size());
  if (x_locations.empty()) {
    // All bits from 0 -> lsb_xs are unknown.
    Bits high_bits = ternary_ops::ToKnownBitsValues(tern.subspan(lsb_xs));
    is.AddInterval(
        Interval::Closed(bits_ops::Concat({high_bits, Bits(lsb_xs)}),
                         bits_ops::Concat({high_bits, Bits::AllOnes(lsb_xs)})));
    is.Normalize();
    return is;
  }

  TernaryVector vec(tern.size() - lsb_xs, TernaryValue::kKnownZero);
  // Copy input ternary from after the last lsb_x.
  std::copy(tern.cbegin() + lsb_xs, tern.cend(), vec.begin());

  Bits high_lsb = Bits::AllOnes(lsb_xs);
  Bits low_lsb(lsb_xs);
  for (const Bits& v : ternary_ops::AllBitsValues(vec)) {
    is.AddInterval(Interval::Closed(bits_ops::Concat({v, low_lsb}),
                                    bits_ops::Concat({v, high_lsb})));
  }
  is.Normalize();
  return is;
}

bool CoversTernary(const Interval& interval, TernarySpan ternary) {
  if (interval.BitCount() != ternary.size()) {
    return false;
  }
  if (ternary_ops::IsFullyKnown(ternary)) {
    return interval.Covers(ternary_ops::ToKnownBitsValues(ternary));
  }
  if (interval.IsPrecise()) {
    return ternary_ops::IsCompatible(ternary, interval.LowerBound());
  }

  Bits lcp = bits_ops::LongestCommonPrefixMSB(
      {interval.LowerBound(), interval.UpperBound()});

  // We know the next bit of the bounds of `interval` differs, and the interval
  // is proper iff the upper bound has a 1 there.
  const bool proper = interval.UpperBound().GetFromMsb(lcp.bit_count());

  TernarySpan prefix = ternary.subspan(ternary.size() - lcp.bit_count());

  // If the interval is proper, then the interval only contains things with
  // this least-common prefix.
  if (proper && !ternary_ops::IsCompatible(prefix, lcp)) {
    return false;
  }

  // If the interval is improper, then it contains everything that doesn't share
  // this prefix. Therefore, unless `prefix` is fully-known and matches the
  // least-common prefix, `ternary` can definitely represent something in the
  // interval.
  if (!proper && !(ternary_ops::IsFullyKnown(prefix) &&
                   ternary_ops::ToKnownBitsValues(prefix) == lcp)) {
    return true;
  }

  // Take the leading value in `ternary`.
  TernaryValue x = ternary[ternary.size() - lcp.bit_count() - 1];

  // Drop all the bits we've already confirmed match, plus one more.
  Bits L = interval.LowerBound().Slice(0, ternary.size() - lcp.bit_count() - 1);
  Bits U = interval.UpperBound().Slice(0, ternary.size() - lcp.bit_count() - 1);
  TernarySpan t = ternary.subspan(0, ternary.size() - lcp.bit_count() - 1);

  auto could_be_le = [](TernarySpan t, const Bits& L) {
    for (int64_t i = t.size() - 1; i >= 0; --i) {
      if (L.Get(i)) {
        if (t[i] != TernaryValue::kKnownOne) {
          // If this bit is zero, it will make t < L.
          return true;
        }
      } else if (t[i] == TernaryValue::kKnownOne) {
        // We know t > L.
        return false;
      }
    }
    return true;
  };
  auto could_be_ge = [](TernarySpan t, const Bits& U) {
    for (int64_t i = t.size() - 1; i >= 0; --i) {
      if (U.Get(i)) {
        if (t[i] == TernaryValue::kKnownZero) {
          // We know t < U.
          return false;
        }
      } else if (t[i] != TernaryValue::kKnownZero) {
        // If this bit is one, it will make t > L.
        return true;
      }
    }
    return true;
  };

  // NOTE: At this point, we want to know:
  //
  //   if improper, whether it's possible to have:
  //     xt <= 0U || 1L <= xt, which is true iff
  //     (x == 0 && t <= U) || (x == 1 && L <= t).
  //
  //   if proper, whether it's possible to have:
  //     0L <= xt && xt <= 1U, which is true iff
  //     (x == 1 || L <= t) && (x == 0 || t <= U).
  //
  // If x is known, then this is easy:
  //   if x == 0 && proper: check if it's possible to have L <= t.
  //   if x == 1 && improper: check if it's possible to have L <= t.
  //   if x == 0 && improper: check if it's possible to have t <= U.
  //   if x == 1 && proper: check if it's possible to have t <= U.
  // In other words:
  //   if (x == 0) == proper, check if it's possible to have L <= t.
  //               Otherwise, check if it's possible to have t <= U.
  if (ternary_ops::IsKnown(x)) {
    if ((x == TernaryValue::kKnownZero) == proper) {
      return could_be_ge(t, L);
    }
    return could_be_le(t, U);
  }

  // If x is unknown, then we can choose whichever value we want. Therefore, we
  // just need to know:
  //   if improper, whether it's possible to have... well.
  //     if we take x == 0, then we just need to check if we can have t <= U.
  //     if we take x == 1, then we just need to check if we can have L <= t.
  //     Therefore, we just need to check whether it's possible to have:
  //       t <= U || L <= t.
  //   if proper, whether it's possible to have... well.
  //     If we take x == 1, then we just need to check if we can have t <= U.
  //     If we take x == 0, then we just need to check if we can have L <= t.
  //     Therefore, we just need to check whether it's possible to have:
  //       t <= U || L <= t.
  // The conclusion is the same whether the interval is proper or improper, so
  // we check this and we're done.
  return could_be_le(t, U) || could_be_ge(t, L);
}

bool CoversTernary(const IntervalSet& intervals, TernarySpan ternary) {
  if (intervals.BitCount() != ternary.size()) {
    return false;
  }
  return absl::c_any_of(intervals.Intervals(),
                        [&ternary](const Interval& interval) {
                          return CoversTernary(interval, ternary);
                        });
}

namespace {
enum class Tonicity : bool { Monotone, Antitone };
// What sort of behavior the argument exhibits
struct ArgumentBehavior {
  // Whether increasing the value of this argument causes the output value to
  // increase (monotone) or decrease (antitone). NB This ignores overflows.
  Tonicity tonicity;

  // Whether the argument is size-preserving; i.e., does changing the argument
  // by 1 cause a change in the output by 1 (either up or down depending on
  // tonicity).
  bool size_preserving;
};
static constexpr ArgumentBehavior kMonotoneSizePreserving{
    .tonicity = Tonicity::Monotone, .size_preserving = true};
static constexpr ArgumentBehavior kMonotoneNonSizePreserving{
    .tonicity = Tonicity::Monotone, .size_preserving = false};
static constexpr ArgumentBehavior kAntitoneSizePreserving{
    .tonicity = Tonicity::Antitone, .size_preserving = true};
static constexpr ArgumentBehavior kAntitoneNonSizePreserving{
    .tonicity = Tonicity::Antitone, .size_preserving = false};

TernaryValue OneBitRangeToTernary(const IntervalSet& is) {
  CHECK_EQ(is.BitCount(), 1);
  if (is.IsPrecise()) {
    return is.CoversZero() ? TernaryValue::kKnownZero : TernaryValue::kKnownOne;
  }
  return TernaryValue::kUnknown;
}

IntervalSet TernaryToOneBitRange(TernaryValue v) {
  switch (v) {
    case TernaryValue::kKnownZero:
      return IntervalSet::Precise(UBits(0, 1));
    case TernaryValue::kKnownOne:
      return IntervalSet::Precise(UBits(1, 1));
    case TernaryValue::kUnknown:
      return IntervalSet::Maximal(1);
  }
}

struct OverflowResult {
  Bits result;
  // Set if overflowed to 'inputs + 1' bits
  bool first_overflow_bit = false;
  // Set if overflowed to 'inputs + 2' bits
  bool second_overflow_bit = false;
};

template <typename Calculate>
  requires(
      std::is_invocable_r_v<OverflowResult, Calculate, absl::Span<Bits const>>)
IntervalSet PerformVariadicOp(Calculate calc,
                              absl::Span<ArgumentBehavior const> behaviors,
                              absl::Span<IntervalSet const> input_operands,
                              int64_t result_bit_size) {
  CHECK_EQ(input_operands.size(), behaviors.size());

  std::vector<IntervalSet> operands;
  operands.reserve(input_operands.size());

  {
    int64_t i = 0;
    for (IntervalSet interval_set : input_operands) {
      // TODO(taktoa): we could choose the minimized interval sets more
      // carefully, since `MinimizeIntervals` is minimizing optimally for each
      // interval set without the knowledge that other interval sets exist.
      // For example, we could call `ConvexHull` greedily on the sets
      // that have the smallest difference between convex hull size and size.

      // TODO(allight): We might want to distribute the intervals more evenly
      // then just giving the first 12 operands 5 segments and the rest 1.
      // Limit exponential growth after 12 parameters. 5^12 = 244 million
      interval_set = MinimizeIntervals(interval_set, (i < 12) ? 5 : 1);
      operands.push_back(interval_set);
      ++i;
    }
  }

  if (absl::c_all_of(operands,
                     [](const IntervalSet& i) { return i.IsPrecise(); })) {
    // All inputs are fully known. The result is the one result of applying the
    // calculation. Overflow doesn't matter since the operation only occurs with
    // one set of values.
    std::vector<Bits> real_values;
    real_values.reserve(operands.size());
    for (const IntervalSet& i : operands) {
      real_values.push_back(*i.GetPreciseValue());
    }
    return IntervalSet::Precise(calc(real_values).result);
  }

  std::vector<int64_t> radix;
  radix.reserve(operands.size());
  for (const IntervalSet& interval_set : operands) {
    radix.push_back(interval_set.NumberOfIntervals());
  }

  IntervalSet result_intervals(result_bit_size);
  bool overflow_is_size_preserving = true;
  int64_t count_non_precise = 0;
  for (int64_t i = 0; overflow_is_size_preserving && i < behaviors.size();
       ++i) {
    overflow_is_size_preserving =
        overflow_is_size_preserving &&
        (behaviors[i].size_preserving || operands[i].IsPrecise());
    if (!operands[i].IsPrecise()) {
      ++count_non_precise;
    }
  }
  // If there's only one non-precise argument and overflow caused by it is
  // size-preserving then overflow of the high-side (or low-side on antitone
  // operation) can't "catch-up" to the low-side meaning that '[f(low) %
  // (1<<bit_count), f(high) % (1 << bit_count)]' is always a valid range.
  overflow_is_size_preserving =
      overflow_is_size_preserving && count_non_precise == 1;

  // Each iteration of this do-while loop explores a different choice of
  // intervals from each interval set associated with a parameter.
  MixedRadixIterate(radix, [&](const std::vector<int64_t>& indexes) -> bool {
    std::vector<Bits> lower_bounds;
    lower_bounds.reserve(indexes.size());
    std::vector<Bits> upper_bounds;
    upper_bounds.reserve(indexes.size());
    for (int64_t i = 0; i < indexes.size(); ++i) {
      Interval interval = operands[i].Intervals()[indexes[i]];
      switch (behaviors[i].tonicity) {
        case Tonicity::Monotone: {
          // The essential property of a unary monotone function `f` is that
          // the codomain of `f` applied to `[x, y]` is `[f(x), f(y)]`.
          // For example, the cubing function applied to `[5, 8]` gives a
          // codomain of `[125, 512]`.
          lower_bounds.push_back(interval.LowerBound());
          upper_bounds.push_back(interval.UpperBound());
          break;
        }
        case Tonicity::Antitone: {
          // The essential property of a unary antitone function `f` is that
          // the codomain of `f` applied to `[x, y]` is `[f(y), f(x)]`.
          // For example, the negation function applied to `[10, 20]` gives
          // a codomain of `[-20, -10]`.
          lower_bounds.push_back(interval.UpperBound());
          upper_bounds.push_back(interval.LowerBound());
          break;
        }
      }
    }
    OverflowResult lower = calc(lower_bounds);
    OverflowResult upper = calc(upper_bounds);
    if (!lower.first_overflow_bit && !upper.first_overflow_bit) {
      // No overflow at all.
      result_intervals.AddInterval(Interval(lower.result, upper.result));
      return false;
    }
    // Size-preserving here means that only a single input is varying. In this
    // case the fact that it's size-preserving means that to overflow twice is
    // impossible since that would mean that there would need to be elements in
    // the output that are not mapped to. Since we are size-preserving the
    // difference between the input and output must remain the same so we can
    // just add the interval.
    if (overflow_is_size_preserving) {
      // Possibly improper interval.
      result_intervals.AddInterval(Interval(lower.result, upper.result));
      return false;
    }
    // Check for overflows that cover the entire output space.
    // If both sides overflowed then its unconstrained.
    if ((lower.first_overflow_bit && upper.first_overflow_bit) ||
        // If either overflowed twice it must be unconstrained.
        lower.second_overflow_bit || upper.second_overflow_bit ||
        // If neither of the other two are true but upper is still larger
        // then lower then one of them must have gone all the way around.
        bits_ops::UGreaterThan(upper.result, lower.result)) {
      // We're unconstrained so no need to continue searching the output
      // space.
      result_intervals.AddInterval(Interval::Maximal(result_bit_size));
      return true;
    }
    // We overflowed on one end of the intervals but not all the way past
    // the other bound so we have an intervals with the inverse of [high,
    // low].
    result_intervals.AddInterval(
        Interval(lower.result, Bits::AllOnes(result_bit_size)));
    result_intervals.AddInterval(Interval(Bits(result_bit_size), upper.result));
    return false;
  });

  result_intervals.Normalize();
  return MinimizeIntervals(result_intervals, /*size=*/16);
}

template <typename Calculate>
  requires(std::is_invocable_r_v<Bits, Calculate, absl::Span<Bits const>>)
IntervalSet PerformVariadicOp(Calculate calc,
                              absl::Span<ArgumentBehavior const> behaviors,
                              absl::Span<IntervalSet const> input_operands,
                              int64_t result_bit_size) {
  return PerformVariadicOp(
      [&](absl::Span<Bits const> a) -> OverflowResult {
        return {.result = calc(a)};
      },
      behaviors, input_operands, result_bit_size);
}

template <typename Calculate>
  requires(std::is_invocable_r_v<OverflowResult, Calculate, const Bits&,
                                 const Bits&>)
IntervalSet PerformBinOp(Calculate calc, const IntervalSet& lhs,
                         ArgumentBehavior lhs_behavior, const IntervalSet& rhs,
                         ArgumentBehavior rhs_behavior,
                         int64_t result_bit_size) {
  return PerformVariadicOp(
      [&](absl::Span<Bits const> bits) -> OverflowResult {
        CHECK_EQ(bits.size(), 2);
        return calc(bits[0], bits[1]);
      },
      {lhs_behavior, rhs_behavior}, {lhs, rhs}, result_bit_size);
}
template <typename Calculate>
  requires(std::is_invocable_r_v<Bits, Calculate, const Bits&, const Bits&>)
IntervalSet PerformBinOp(Calculate calc, const IntervalSet& lhs,
                         ArgumentBehavior lhs_behavior, const IntervalSet& rhs,
                         ArgumentBehavior rhs_behavior,
                         int64_t result_bit_size) {
  return PerformBinOp(
      [&calc](const Bits& l, const Bits& r) -> OverflowResult {
        return {.result = calc(l, r)};
      },
      lhs, lhs_behavior, rhs, rhs_behavior, result_bit_size);
}

template <typename Calculate>
  requires(std::is_invocable_r_v<OverflowResult, Calculate, const Bits&>)
IntervalSet PerformUnaryOp(Calculate calc, const IntervalSet& arg,
                           ArgumentBehavior behavior, int64_t result_bit_size) {
  return PerformVariadicOp(
      [&](absl::Span<Bits const> bits) -> OverflowResult {
        CHECK_EQ(bits.size(), 1);
        return calc(bits[0]);
      },
      {behavior}, {arg}, result_bit_size);
}

template <typename Calculate>
  requires(std::is_invocable_r_v<Bits, Calculate, const Bits&>)
IntervalSet PerformUnaryOp(Calculate calc, const IntervalSet& arg,
                           ArgumentBehavior behavior, int64_t result_bit_size) {
  return PerformUnaryOp(
      [&calc](const Bits& b) -> OverflowResult {
        return OverflowResult{.result = calc(b)};
      },
      arg, behavior, result_bit_size);
}

// An intrusive list node of an interval list
struct MergeInterval {
  Interval final_interval;
  Bits gap_with_previous;
  // Intrusive list links. Next & previous lexicographic interval.
  MergeInterval* prev = nullptr;
  MergeInterval* next = nullptr;

  friend std::strong_ordering operator<=>(const MergeInterval& l,
                                          const MergeInterval& r) {
    auto cmp_bits = [](const Bits& l, const Bits& r) {
      return bits_ops::UCmp(l, r) <=> 0;
    };
    std::strong_ordering gap_order =
        cmp_bits(l.gap_with_previous, r.gap_with_previous);
    if (gap_order != std::strong_ordering::equal) {
      return gap_order;
    }
    return cmp_bits(l.final_interval.LowerBound(),
                    r.final_interval.LowerBound());
  }
};

}  // namespace

// Minimize interval set to 'size' by merging some intervals together. Intervals
// are chosen with a greedy algorithm that minimizes the number of additional
// values the overall interval set contains. That is first it will add the
// smallest components posible. In cases where multiple gaps are the same size
// it will prioritize earlier gaps over later ones.
IntervalSet MinimizeIntervals(IntervalSet interval_set, int64_t size) {
  interval_set.Normalize();

  // Check for easy cases (already small enough and convex hull)
  if (interval_set.NumberOfIntervals() <= size) {
    return interval_set;
  }
  if (size == 1) {
    IntervalSet res(interval_set.BitCount());
    res.AddInterval(*interval_set.ConvexHull());
    res.Normalize();
    return res;
  }

  std::vector<std::unique_ptr<MergeInterval>> merge_list;
  merge_list.reserve(interval_set.NumberOfIntervals() - 1);
  // The first one will never get merged with the previous since that wouldn't
  // actually remove an interval segment so we don't include it on the merge
  // list. Things can get merged into it however.
  DCHECK(absl::c_is_sorted(interval_set.Intervals()));
  MergeInterval first{.final_interval = interval_set.Intervals().front()};
  for (auto it = interval_set.Intervals().begin() + 1;
       it != interval_set.Intervals().end(); ++it) {
    MergeInterval* prev = merge_list.empty() ? &first : merge_list.back().get();
    Bits distance = bits_ops::Sub(it->LowerBound(), (it - 1)->UpperBound());
    // Generate a list with an intrusive list containing the original
    // ordering.
    merge_list.push_back(std::make_unique<MergeInterval>(
        MergeInterval{.final_interval = *it,
                      .gap_with_previous = std::move(distance),
                      .prev = prev}));
    prev->next = merge_list.back().get();
  }

  // We want a min-heap so cmp is greater-than.
  auto heap_cmp = [](const std::unique_ptr<MergeInterval>& l,
                     const std::unique_ptr<MergeInterval>& r) {
    return *l > *r;
  };
  // make the merge_list a heap.
  absl::c_make_heap(merge_list, heap_cmp);

  // Remove the minimum element from the merge_list heap.
  auto pop_min_element = [&]() -> std::unique_ptr<MergeInterval> {
    absl::c_pop_heap(merge_list, heap_cmp);
    std::unique_ptr<MergeInterval> minimum = std::move(merge_list.back());
    merge_list.pop_back();
    return minimum;
  };
  // Merge elements until we are the appropriate size.
  // NB Since the first interval isn't in the heap (since it can't get merged
  // from) we need to continue until the heap is one element shorter than
  // requested.
  while (merge_list.size() > size - 1) {
    // Pull the item with the smallest distance
    std::unique_ptr<MergeInterval> min_interval = pop_min_element();
    // Merge with the prior element.

    // extend the previous interval.
    min_interval->prev->final_interval =
        Interval(min_interval->prev->final_interval.LowerBound(),
                 min_interval->final_interval.UpperBound());
    // Update the intrusive list of active merges.
    min_interval->prev->next = min_interval->next;
    if (min_interval->next != nullptr) {
      min_interval->next->prev = min_interval->prev;
    }
  }

  // Now 'first, ...merge_list' is `size` elements.
  IntervalSet result;
  std::vector<Interval> final_intervals{std::move(first.final_interval)};
  final_intervals.reserve(size);
  for (std::unique_ptr<MergeInterval>& mi : merge_list) {
    final_intervals.push_back(std::move(mi->final_interval));
  }
  result.SetIntervals(final_intervals);
  result.Normalize();
  return result;
}

IntervalSet Add(const IntervalSet& a, const IntervalSet& b) {
  return PerformBinOp(
      [](const Bits& lhs, const Bits& rhs) -> OverflowResult {
        int64_t padded_size = std::max(lhs.bit_count(), rhs.bit_count()) + 1;
        Bits padded_lhs = bits_ops::ZeroExtend(lhs, padded_size);
        Bits padded_rhs = bits_ops::ZeroExtend(rhs, padded_size);
        Bits padded_result = bits_ops::Add(padded_lhs, padded_rhs);
        // If the MSB is 1, then we overflowed.
        bool overflow = padded_result.GetFromMsb(0);
        return OverflowResult{
            .result =
                bits_ops::Truncate(std::move(padded_result), padded_size - 1),
            .first_overflow_bit = overflow,
            .second_overflow_bit = false,
        };
      },
      a, kMonotoneSizePreserving, b, kMonotoneSizePreserving, a.BitCount());
}
IntervalSet Sub(const IntervalSet& a, const IntervalSet& b) {
  return PerformBinOp(
      [](const Bits& lhs, const Bits& rhs) -> OverflowResult {
        // x - y overflows if x < y
        return {.result = bits_ops::Sub(lhs, rhs),
                .first_overflow_bit = bits_ops::ULessThan(lhs, rhs)};
      },
      a, kMonotoneSizePreserving, b, kAntitoneSizePreserving, a.BitCount());
}
IntervalSet Neg(const IntervalSet& a) {
  return PerformUnaryOp(bits_ops::Negate, a, kAntitoneSizePreserving,
                        a.BitCount());
}
IntervalSet UMul(const IntervalSet& a, const IntervalSet& b,
                 int64_t output_bitwidth) {
  return PerformBinOp(
      [output_bitwidth](const Bits& lhs, const Bits& rhs) -> OverflowResult {
        Bits result = bits_ops::UMul(lhs, rhs);
        int64_t msb_set_bit =
            result.bit_count() - result.CountLeadingZeros() - 1;
        return {.result = Bits::FromBitmap(std::move(result).bitmap().WithSize(
                    output_bitwidth, /*new_data=*/false)),
                .first_overflow_bit = msb_set_bit >= output_bitwidth,
                .second_overflow_bit = msb_set_bit >= output_bitwidth + 1};
      },
      a, kMonotoneNonSizePreserving, b, kMonotoneNonSizePreserving,
      output_bitwidth);
}
IntervalSet UDiv(const IntervalSet& a, const IntervalSet& b) {
  // Integer division is antitone on the second argument since
  // `\forall x,y \in \real: y > 1 \implies x / y <= x`. The one unsigned
  // integer value for which this implication does not hold is `0`. Our UDiv
  // implementation is defined such that UDiv(x, 0) == MAX_int so in cases where
  // zero is possible we add that in.
  if (!b.CoversZero()) {
    return PerformBinOp(bits_ops::UDiv, a, kMonotoneNonSizePreserving, b,
                        kAntitoneNonSizePreserving, a.BitCount());
  }
  IntervalSet nonzero_divisor =
      IntervalSet::Intersect(b, IntervalSet::NonZero(b.BitCount()));
  IntervalSet results(a.BitCount());
  if (!nonzero_divisor.IsEmpty()) {
    // We aren't *only* dividing by zero. Get the non-zero divisor ranges.
    results =
        PerformBinOp(bits_ops::UDiv, a, kMonotoneNonSizePreserving,
                     nonzero_divisor, kAntitoneNonSizePreserving, a.BitCount());
  }
  // Stick in the single value that division by zero yields.
  results.AddInterval(Interval::Precise(Bits::AllOnes(a.BitCount())));
  results.Normalize();
  return results;
}
IntervalSet Shrl(const IntervalSet& a, const IntervalSet& b) {
  return PerformBinOp(
      [](const Bits& lhs, const Bits& rhs) -> OverflowResult {
        absl::StatusOr<uint64_t> shift_amount = rhs.ToUint64();
        if (!shift_amount.ok() ||
            *shift_amount >
                static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
          // We must be overshifting; the result is zero.
          return {.result = Bits(lhs.bit_count())};
        }
        return {.result = bits_ops::ShiftRightLogical(lhs, *shift_amount)};
      },
      a, kMonotoneNonSizePreserving, b, kAntitoneNonSizePreserving,
      a.BitCount());
}
IntervalSet Decode(const IntervalSet& a, int64_t width) {
  IntervalSet result(width);
  // We step through the possible values `i` in `a`, up to `width`, and generate
  // the corresponding power-of-two as an interval. The good news is that we can
  // (and do) stop as soon as we reach `width` or any larger value, since all of
  // those encode the same point (0)... so this visits at most `width` elements.
  a.ForEachElement([&](const Bits& b) {
    uint64_t bit = b.ToUint64().value_or(width);
    if (bit >= static_cast<uint64_t>(width)) {
      // We're done; no need to check later elements.
      result.AddInterval(Interval::Precise(UBits(0, width)));
      return true;
    }

    result.AddInterval(
        Interval::Precise(Bits::PowerOfTwo(static_cast<int64_t>(bit), width)));
    return false;
  });
  result.Normalize();
  return result;
}
IntervalSet SignExtend(const IntervalSet& a, int64_t width) {
  return PerformUnaryOp(
      [&](const Bits& b) -> Bits { return bits_ops::SignExtend(b, width); }, a,
      kMonotoneSizePreserving, width);
}
IntervalSet ZeroExtend(const IntervalSet& a, int64_t width) {
  return PerformUnaryOp(
      [&](const Bits& b) -> Bits { return bits_ops::ZeroExtend(b, width); }, a,
      kMonotoneSizePreserving, width);
}
IntervalSet Truncate(const IntervalSet& a, int64_t width) {
  IntervalSet result(width);
  Bits output_space = Bits::AllOnes(width);
  for (const Interval& i : a.Intervals()) {
    if (bits_ops::UGreaterThan(bits_ops::Sub(i.UpperBound(), i.LowerBound()),
                               output_space)) {
      // Interval covers everything.
      return IntervalSet::Maximal(width);
    }
    Bits low = i.LowerBound().Slice(0, width);
    Bits high = i.UpperBound().Slice(0, width);
    // NB Improper intervals are automatically split once we do normalization.
    result.AddInterval(Interval(low, high));
  }
  result.Normalize();
  return result;
}
IntervalSet BitSlice(const IntervalSet& a, int64_t start, int64_t width) {
  return Truncate(Shrl(a, IntervalSet::Precise(UBits(start, 64))), width);
}

IntervalSet Concat(absl::Span<IntervalSet const> sets) {
  std::vector<ArgumentBehavior> behaviors(sets.size(),
                                          kMonotoneNonSizePreserving);
  behaviors.back() = kMonotoneSizePreserving;
  return PerformVariadicOp(
      bits_ops::Concat, behaviors, sets,
      absl::c_accumulate(
          sets, int64_t{0},
          [](int64_t v, const IntervalSet& is) { return v + is.BitCount(); }));
}

// Bit ops.
IntervalSet Not(const IntervalSet& a) {
  TernaryEvaluator eval;
  // Special case 1-bit version to avoid allocations.
  if (a.BitCount() == 1) {
    return TernaryToOneBitRange(eval.Not(OneBitRangeToTernary(a)));
  }
  TernaryVector vec = ExtractTernaryVector(a);
  TernaryVector res = eval.BitwiseNot(vec);
  return FromTernary(res);
}
IntervalSet And(const IntervalSet& a, const IntervalSet& b) {
  CHECK_EQ(a.BitCount(), b.BitCount());
  // Special case 1-bit version to avoid allocations.
  TernaryEvaluator eval;
  if (a.BitCount() == 1) {
    return TernaryToOneBitRange(
        eval.And(OneBitRangeToTernary(a), OneBitRangeToTernary(b)));
  }
  TernaryVector res =
      eval.BitwiseAnd(ExtractTernaryVector(a), ExtractTernaryVector(b));
  return FromTernary(res);
}
IntervalSet Or(const IntervalSet& a, const IntervalSet& b) {
  CHECK_EQ(a.BitCount(), b.BitCount());
  TernaryEvaluator eval;
  if (a.BitCount() == 1) {
    return TernaryToOneBitRange(
        eval.Or(OneBitRangeToTernary(a), OneBitRangeToTernary(b)));
  }
  TernaryVector res =
      eval.BitwiseOr(ExtractTernaryVector(a), ExtractTernaryVector(b));
  return FromTernary(res);
}

IntervalSet Xor(const IntervalSet& a, const IntervalSet& b) {
  CHECK_EQ(a.BitCount(), b.BitCount());
  TernaryEvaluator eval;
  if (a.BitCount() == 1) {
    return TernaryToOneBitRange(
        eval.Xor(OneBitRangeToTernary(a), OneBitRangeToTernary(b)));
  }
  TernaryVector res =
      eval.BitwiseXor(ExtractTernaryVector(a), ExtractTernaryVector(b));
  return FromTernary(res);
}

IntervalSet AndReduce(const IntervalSet& a) {
  // Unless the intervals cover max, the and_reduce of the input must be 0.
  if (!a.CoversMax()) {
    return TernaryToOneBitRange(TernaryValue::kKnownZero);
  }
  // If the intervals is precise and covers max it must be 1.
  if (a.IsPrecise()) {
    return TernaryToOneBitRange(TernaryValue::kKnownOne);
  }
  // Not knowable
  return TernaryToOneBitRange(TernaryValue::kUnknown);
}

IntervalSet OrReduce(const IntervalSet& a) {
  // Unless the intervals cover 0, the or_reduce of the input must be 1.
  if (!a.CoversZero()) {
    return TernaryToOneBitRange(TernaryValue::kKnownOne);
  }
  // If the intervals are known to only cover 0, then the result must be 0.
  if (a.IsPrecise()) {
    return TernaryToOneBitRange(TernaryValue::kKnownZero);
  }
  // Not knowable
  return TernaryToOneBitRange(TernaryValue::kUnknown);
}
IntervalSet XorReduce(const IntervalSet& a) {
  // XorReduce determines the parity of the number of 1s in a bitstring.
  // Incrementing a bitstring always outputs in a bitstring with a different
  // parity of 1s (since even + 1 = odd and odd + 1 = even). Therefore, this
  // analysis cannot return anything but unknown when an interval is
  // imprecise. When the given set of intervals only contains precise
  // intervals, we can check whether they all have the same parity of 1s, and
  // return 1 or 0 if they are all the same, or unknown otherwise.
  if (a.NumberOfIntervals() == 0 || !a.Intervals().front().IsPrecise()) {
    return TernaryToOneBitRange(TernaryValue::kUnknown);
  }
  Bits output = bits_ops::XorReduce(*a.Intervals().front().GetPreciseValue());
  for (const Interval& interval :
       absl::MakeConstSpan(a.Intervals()).subspan(1)) {
    if (!interval.IsPrecise() ||
        bits_ops::XorReduce(*interval.GetPreciseValue()) != output) {
      return TernaryToOneBitRange(TernaryValue::kUnknown);
    }
  }
  return TernaryToOneBitRange(output.IsOne() ? TernaryValue::kKnownOne
                                             : TernaryValue::kKnownZero);
}

// Cmp
IntervalSet Eq(const IntervalSet& a, const IntervalSet& b) {
  if (a.IsPrecise() && b.IsPrecise()) {
    return TernaryToOneBitRange(
        bits_ops::UEqual(*a.GetPreciseValue(), *b.GetPreciseValue())
            ? TernaryValue::kKnownOne
            : TernaryValue::kKnownZero);
  }

  return TernaryToOneBitRange(IntervalSet::Disjoint(a, b)
                                  ? TernaryValue::kKnownZero
                                  : TernaryValue::kUnknown);
}

IntervalSet Ne(const IntervalSet& a, const IntervalSet& b) {
  return Not(Eq(a, b));
}

IntervalSet ULt(const IntervalSet& a, const IntervalSet& b) {
  Interval lhs_hull = *a.ConvexHull();
  Interval rhs_hull = *b.ConvexHull();
  if (Interval::Disjoint(lhs_hull, rhs_hull)) {
    return TernaryToOneBitRange(lhs_hull < rhs_hull ? TernaryValue::kKnownOne
                                                    : TernaryValue::kKnownZero);
  }
  return TernaryToOneBitRange(TernaryValue::kUnknown);
}

IntervalSet UGt(const IntervalSet& a, const IntervalSet& b) {
  Interval lhs_hull = *a.ConvexHull();
  Interval rhs_hull = *b.ConvexHull();
  if (Interval::Disjoint(lhs_hull, rhs_hull)) {
    return TernaryToOneBitRange(lhs_hull > rhs_hull ? TernaryValue::kKnownOne
                                                    : TernaryValue::kKnownZero);
  }
  return TernaryToOneBitRange(TernaryValue::kUnknown);
}

IntervalSet SLt(const IntervalSet& a, const IntervalSet& b) {
  CHECK(a.IsNormalized());
  CHECK(b.IsNormalized());
  auto is_all_negative = [](const IntervalSet& v) {
    return v.LowerBound()->GetFromMsb(0) && v.UpperBound()->GetFromMsb(0);
  };
  auto is_all_positive = [](const IntervalSet& v) {
    return !v.LowerBound()->GetFromMsb(0) && !v.UpperBound()->GetFromMsb(0);
  };
  // Avoid doing the add if possible.
  if ((is_all_positive(a) && is_all_positive(b)) ||
      (is_all_negative(a) && is_all_negative(b))) {
    // Entire range is positive or negative on both args. Can do an unsigned
    // compare.
    return ULt(a, b);
  }
  IntervalSet offset = IntervalSet::Precise(
      bits_ops::Concat({UBits(1, 1), Bits(a.BitCount() - 1)}));
  return ULt(Add(a, offset), Add(b, offset));
}

IntervalSet SGt(const IntervalSet& a, const IntervalSet& b) {
  CHECK(a.IsNormalized());
  CHECK(b.IsNormalized());
  auto is_all_negative = [](const IntervalSet& v) {
    return v.LowerBound()->GetFromMsb(0) && v.UpperBound()->GetFromMsb(0);
  };
  auto is_all_positive = [](const IntervalSet& v) {
    return !v.LowerBound()->GetFromMsb(0) && !v.UpperBound()->GetFromMsb(0);
  };
  // Avoid doing the add if possible.
  if ((is_all_positive(a) && is_all_positive(b)) ||
      (is_all_negative(a) && is_all_negative(b))) {
    // Entire range is positive or negative on both args. Can do an unsigned
    // compare.
    return UGt(a, b);
  }
  // Offset by signed-max and compare.
  IntervalSet offset = IntervalSet::Precise(
      bits_ops::Concat({UBits(1, 1), Bits(a.BitCount() - 1)}));
  return UGt(Add(a, offset), Add(b, offset));
}

IntervalSet Gate(const IntervalSet& cond, const IntervalSet& val) {
  if (cond.IsPrecise()) {
    if (cond.CoversZero()) {
      return IntervalSet::Precise(Bits(val.BitCount()));
    }
    return val;
  }
  if (cond.CoversZero()) {
    // has zero and some other value so mix.
    return IntervalSet::Combine(val,
                                IntervalSet::Precise(Bits(val.BitCount())));
  }
  // Definitely not zero.
  return val;
}
IntervalSet OneHot(const IntervalSet& val, LsbOrMsb lsb_or_msb,
                   int64_t max_interval_bits) {
  TernaryEvaluator tern;
  TernaryVector src = ExtractTernaryVector(val);
  TernaryVector res;
  switch (lsb_or_msb) {
    case LsbOrMsb::kLsb:
      res = tern.OneHotLsbToMsb(src);
      break;
    case LsbOrMsb::kMsb:
      res = tern.OneHotMsbToLsb(src);
      break;
  }
  return FromTernary(res, max_interval_bits);
}

}  // namespace xls::interval_ops
