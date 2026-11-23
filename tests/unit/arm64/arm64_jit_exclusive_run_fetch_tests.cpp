// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_exclusive_run_support.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_exclusive_run.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace uml::emulator::tests;
namespace arm64 = uml::cpu::arm64;

/*
 * The span of words the executor reads for a block (the look-ahead of a lock
 * sequence to its store-exclusive), judged on word tables through the
 * production function, and what each hot lock sequence then compiles to
 * through run_block.
 */

namespace {

constexpr std::uint64_t page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr std::uint64_t origin = 0x7000'0000U;

/* Guest words from `start`, with every access recorded. */
struct Image final {
  std::uint64_t start{};
  std::vector<std::uint32_t> words;
  std::vector<std::uint64_t> fetched;

  bool operator()(std::uint64_t address, std::uint32_t &word) {
    fetched.push_back(address);
    if (address < start || (address - start) % 4U != 0U ||
        (address - start) / 4U >= words.size())
      return false;
    word = words[(address - start) / 4U];
    return true;
  }
  [[nodiscard]] std::uint64_t highest_fetch() const {
    return *std::max_element(fetched.begin(), fetched.end());
  }
};

struct Span final {
  std::size_t count{};
  std::vector<std::uint32_t> words;
  std::uint64_t highest{};
};

[[nodiscard]] Span fetch_span(std::uint64_t pc,
                              std::vector<std::uint32_t> words) {
  Image image{pc, std::move(words), {}};
  std::array<std::uint32_t, arm64::maximum_block_instructions> out{};
  Span span;
  span.count = arm64::fetch_translation_words(pc, out, image);
  span.words.assign(out.begin(), out.begin() + span.count);
  span.highest = image.fetched.empty() ? 0 : image.highest_fetch();
  return span;
}

/* `words` then a run of system calls, so that nothing past the table fails
 * a fetch unless a row wants it to. */
[[nodiscard]] std::vector<std::uint32_t>
padded(std::vector<std::uint32_t> words) {
  words.resize(words.size() + 16U, a64::system_call);
  return words;
}

using namespace a64;
constexpr std::uint32_t load_x3 = load_exclusive(3, false, 3, 1);
constexpr std::uint32_t store_x5 = store_exclusive(3, false, 5, 2, 1);
constexpr std::uint32_t retry_x5 = compare_branch(false, true, 5, -4);

void each_hot_shape_reaches_its_store_and_one_word_more() {
  for (const auto *shape : hot_shapes) {
    std::vector<std::uint32_t> words(shape->words.begin(),
                                     shape->words.begin() + shape->length);
    words.push_back(ret);
    const auto span = fetch_span(origin, padded(words));
    require(span.count == shape->length);
    require(arm64::exclusive_run_word_kind(
                span.words[shape->store_index]) ==
            arm64::ExclusiveRunWord::store_exclusive);
    /* Nothing past the retry is read. */
    require(span.highest == origin + (shape->length - 1U) * 4U);
  }
}

void a_four_middle_run_is_accepted_whole() {
  const std::vector<std::uint32_t> branch_last{
      load_x3, movz(true, 6, 1),       add_sub_immediate(true, false, false, 7, 6, 1),
      logical_register(true, 2, false, 6, 7, 3),
      branch_condition(eq, 20), store_x5, retry_x5};
  const auto last = fetch_span(origin, padded(branch_last));
  require(last.count == 7U && arm64::maximum_exclusive_run_words == 7U);
  require(last.highest == origin + 6U * 4U);

  /* The first conditional branch right after the load, four middles in all. */
  const std::vector<std::uint32_t> branch_first{
      load_x3, compare_branch(true, false, 3, 20),
      movz(true, 6, 1), movk(true, 6, 2, 1), add_sub_immediate(true, false, false, 7, 6, 1),
      store_x5, retry_x5};
  const auto first = fetch_span(origin, padded(branch_first));
  require(first.count == 7U);

  /* No store among words 1 to 5: the span falls back to the ordinary one. */
  const std::vector<std::uint32_t> five_middles{
      load_x3, compare_branch(true, false, 3, 20),
      movz(true, 6, 1), movk(true, 6, 2, 1), movk(true, 6, 3, 2),
      add_sub_immediate(true, false, false, 7, 6, 1), store_x5, retry_x5};
  const auto exhausted = fetch_span(origin, padded(five_middles));
  require(exhausted.count == 2U);
}

void other_words_keep_the_ordinary_span() {
  /* Not an admissible word after the branch. */
  require(fetch_span(origin, padded({load_x3, cmp(true, 3, 4),
                                     branch_condition(ne, 12),
                                     load_x(7, 1), store_x5, retry_x5}))
              .count == 3U);
  /* An unconditional terminator, an indirect branch and a call. */
  require(fetch_span(origin, padded({load_x3, cmp(true, 3, 4),
                                     branch_condition(ne, 12), branch(8),
                                     store_x5, retry_x5}))
              .count == 3U);
  require(fetch_span(origin, padded({load_x3, compare_branch(true, false, 3, 8),
                                     ret, store_x5, retry_x5}))
              .count == 2U);
  /* The always conditions are not side exits. */
  require(fetch_span(origin, padded({load_x3, cmp(true, 3, 4),
                                     branch_condition(al, 12), store_x5,
                                     retry_x5}))
              .count == 3U);
  /* A word of the ordinary span that no run holds. */
  require(fetch_span(origin, padded({load_x3, load_x(7, 1),
                                     compare_branch(true, false, 3, 8),
                                     store_x5, retry_x5}))
              .count == 3U);
  /* A pair, a plain load and a non-exclusive start. */
  require(fetch_span(origin, padded({0xc87f'0c22U, cmp(true, 3, 4),
                                     branch_condition(ne, 12), store_x5,
                                     retry_x5}))
              .count == 3U);
  require(fetch_span(origin, padded({add_sub_immediate(true, false, false, 3, 3, 1),
                                     branch_condition(ne, 12), store_x5,
                                     retry_x5}))
              .count == 2U);
  require(fetch_span(origin, padded({nop, nop, compare_branch(true, true, 1, 9),
                                     store_x5}))
              .count == 3U);
}

void a_span_that_holds_its_store_is_never_extended() {
  const auto swap = fetch_span(origin, padded({load_x3,
      add_sub_immediate(true, false, false, 3, 3, 1), store_x5, retry_x5,
      store_x5, retry_x5}));
  require(swap.count == 4U && swap.highest == origin + 3U * 4U);
  const auto plain = fetch_span(origin, padded({load_x3, store_x5, retry_x5}));
  require(plain.count == 3U && plain.highest == origin + 2U * 4U);
  /* A store before the branch, another after it. */
  const auto two = fetch_span(origin, padded({load_x3, store_x5,
      compare_branch(true, false, 3, 4), store_x5, retry_x5}));
  require(two.count == 3U && two.highest == origin + 2U * 4U);
}

void the_span_stops_at_the_page_of_the_load() {
  /* A compare-retry whose load is one of the page's last three words and
   * whose store and retry lie on the next page: the words of the load's page
   * alone give the ordinary span, which ends at the branch, and nothing past
   * the branch is read. */
  const std::vector<std::uint32_t> compare_retry{
      load_x3, cmp(true, 3, 4), branch_condition(ne, 12), store_x5, retry_x5};
  for (const unsigned back : {12U, 8U, 4U}) {
    const auto start = origin + page - back;
    const auto span = fetch_span(start, padded(compare_retry));
    require(span.count == 3U && span.highest == start + 2U * 4U);
  }
  /* The store is the page's last word: the retry would lie past it. */
  const auto stored_start = origin + page - 4U * 4U;
  const auto stored = fetch_span(stored_start, padded(compare_retry));
  require(stored.count == 3U && stored.highest == stored_start + 3U * 4U);
  /* The whole run in the page: accepted, retry at the page's last word. */
  const auto fit_start = origin + page - 5U * 4U;
  const auto fit = fetch_span(fit_start, padded(compare_retry));
  require(fit.count == 5U && fit.highest == origin + page - 4U);
}

void a_fetch_that_fails_leaves_the_ordinary_span() {
  /* The table ends before the store, and before the retry. */
  const std::vector<std::uint32_t> cut_before_store{
      load_x3, cmp(true, 3, 4), branch_condition(ne, 12)};
  require(fetch_span(origin, cut_before_store).count == 3U);
  const std::vector<std::uint32_t> cut_before_retry{
      load_x3, cmp(true, 3, 4), branch_condition(ne, 12), store_x5};
  require(fetch_span(origin, cut_before_retry).count == 3U);
  const std::vector<std::uint32_t> cut_in_the_middle{
      load_x3, compare_branch(true, false, 3, 12), movz(true, 6, 1)};
  require(fetch_span(origin, cut_in_the_middle).count == 2U);
  /* No word at all. */
  require(fetch_span(origin, {}).count == 0U);
}

void identical_words_give_identical_spans() {
  const std::vector<std::uint32_t> words = padded(
      {load_x3, cmp(true, 3, 4), branch_condition(ne, 12), store_x5,
       retry_x5});
  const auto first = fetch_span(origin, words);
  const auto second = fetch_span(origin + 7U * page, words);
  require(first.count == 5U && second.count == first.count &&
          first.words == second.words);
  /* An ordinary block is unchanged. */
  const std::vector<std::uint32_t> ordinary = padded(
      {add_sub_immediate(true, false, false, 3, 3, 1), nop,
       branch_condition(ne, 12)});
  require(fetch_span(origin, ordinary).count == 3U);
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(arm64::set_jit_cache_budget(256U * 1024U * 1024U, 2));
  each_hot_shape_reaches_its_store_and_one_word_more();
  a_four_middle_run_is_accepted_whole();
  other_words_keep_the_ordinary_span();
  a_span_that_holds_its_store_is_never_extended();
  the_span_stops_at_the_page_of_the_load();
  a_fetch_that_fails_leaves_the_ordinary_span();
  identical_words_give_identical_spans();
  return 0;
}
