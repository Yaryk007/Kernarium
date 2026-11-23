// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/gadget_generation_window.hpp"
#include "require.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cache = uml::cpu::gadget_cache;

namespace {

using uml::testing::require;

/* The translation space is no part of the window's bookkeeping. */
using State = cache::GenerationState<int, unsigned long>;

constexpr std::size_t limit = 3;
using Window = cache::GenerationWindow<unsigned long, State, limit>;

/* What a cache does on first sight of an address space, less its
 * translation space. */
State &admit(Window &window, unsigned long generation)
{
	auto [entry, inserted] = window.generations.try_emplace(generation);
	require(inserted);
	window.enroll(entry->second, generation);
	window.touch(generation);
	return entry->second;
}

/* And what it does to retire one, less its slots. */
void retire(Window &window, unsigned long generation, bool tombstone = false)
{
	const auto owner = window.generations.find(generation);
	if (owner != window.generations.end()) {
		window.reserve_id();
		window.leave(owner, owner->second.id);
	}
	if (!tombstone)
		window.forget_place(generation);
}

/* An address space nobody runs. */
constexpr auto idle = [](const State &) { return cache::PlaceUse::idle; };

/* Trim the window, retiring as a cache does, and say what went in order. */
template <typename Use>
std::vector<unsigned long> trim(Window &window, Use &&use)
{
	std::vector<unsigned long> retired;
	window.trim(
		[&](unsigned long generation) {
			retired.push_back(generation);
			retire(window, generation);
		},
		use);
	return retired;
}

void ids_count_from_one_and_are_recycled_last_in_first_out()
{
	Window window;
	require(window.owners.size() == 1 && window.owners[0] == nullptr);
	auto &first = admit(window, 100);
	auto &second = admit(window, 200);
	auto &third = admit(window, 300);
	require(first.id == 1 && second.id == 2 && third.id == 3);
	require(first.value == 100 && third.value == 300);
	require(window.owners[1] == &first && window.owners[3] == &third);

	retire(window, 100);
	retire(window, 300);
	require(window.owners[1] == nullptr && window.owners[3] == nullptr);
	require(window.owners[2] == &second);
	/* The id retired last is the first handed out again. */
	require(admit(window, 400).id == 3);
	require(admit(window, 500).id == 1);
	require(admit(window, 600).id == 4);
}

void the_last_id_a_tag_can_carry_is_handed_out()
{
	Window window;
	std::vector<State *> states;
	for (unsigned long generation = 1; generation <= cache::max_owner_id;
	     ++generation) {
		auto [entry, inserted] = window.generations.try_emplace(generation);
		require(inserted);
		window.enroll(entry->second, generation);
		states.push_back(&entry->second);
	}
	require(states.back()->id == cache::max_owner_id);
	/* No owner id is one a tag cannot hold, and the unbound owner bits are
	 * none of them. */
	window.bind(*states.back());
	require((window.current_owner >> cache::tag_owner_shift) ==
		cache::max_owner_id);
	require(window.current_owner != cache::no_owner);
	require((cache::no_owner >> cache::tag_owner_shift) ==
		cache::max_owner_id + 1U);
}

void binding_names_the_owner_and_the_epoch_a_lookup_compares()
{
	Window window;
	require(window.current == nullptr && window.current_epoch == 0 &&
		window.current_owner == cache::no_owner);
	auto &first = admit(window, 7);
	auto &second = admit(window, 8);
	second.epoch = 5;

	window.current = &first;
	window.bind(first);
	require(window.current_owner ==
		std::uint64_t{first.id} << cache::tag_owner_shift);
	require(window.current_epoch == 1);
	window.current = &second;
	window.bind(second);
	require(window.current_owner ==
		std::uint64_t{second.id} << cache::tag_owner_shift);
	require(window.current_epoch == 5);

	window.unbind();
	require(window.current == nullptr && window.current_epoch == 0 &&
		window.current_owner == cache::no_owner);

	/* Retiring the running address space unbinds it; another's retirement
	 * leaves the binding alone. */
	window.current = &first;
	window.bind(first);
	retire(window, 8);
	require(window.current == &first &&
		window.current_owner ==
			std::uint64_t{first.id} << cache::tag_owner_shift);
	retire(window, 7);
	require(window.current == nullptr &&
		window.current_owner == cache::no_owner &&
		window.current_epoch == 0);
}

void an_epoch_wrap_clears_the_validations_it_would_repeat()
{
	Window window;
	auto &state = admit(window, 1);
	auto &other = admit(window, 2);
	window.current = &state;
	window.bind(state);

	unsigned forgotten = 0;
	const auto forget = [&](State &forgetting) noexcept {
		require(&forgetting == &state);
		++forgotten;
	};
	for (unsigned change = 2; change <= cache::max_epoch; ++change) {
		window.advance_epoch(state, forget);
		require(state.epoch == change && window.current_epoch == change);
	}
	require(state.epoch == cache::max_epoch && forgotten == 0);

	/* The change that would pass the maximum clears first and starts over
	 * from one, which is not the epoch of a slot that was cleared (zero). */
	window.advance_epoch(state, forget);
	require(forgotten == 1 && state.epoch == 1 && window.current_epoch == 1);

	/* An address space that is not running moves only its own count. */
	window.advance_epoch(state, forget);
	require(state.epoch == 2 && window.current_epoch == 2);
	other.epoch = cache::max_epoch;
	window.advance_epoch(other, [&](State &forgetting) noexcept {
		require(&forgetting == &other);
		++forgotten;
	});
	require(forgotten == 2 && other.epoch == 1 && state.epoch == 2 &&
		window.current_epoch == 2);
	window.advance_epoch(other, forget);
	require(other.epoch == 2 && window.current_epoch == 2);
}

void the_order_keeps_the_least_recently_current_first()
{
	Window window;
	std::vector<unsigned long> retired;
	const auto trim_window = [&] {
		const auto went = trim(window, idle);
		retired.insert(retired.end(), went.begin(), went.end());
	};
	for (unsigned long generation = 1; generation <= limit; ++generation)
		admit(window, generation);
	trim_window();
	require(retired.empty() && window.generations.size() == limit);

	/* Current again, 1 is the most recent and 2 the oldest. */
	window.touch(1);
	require((window.order == std::vector<unsigned long>{2, 3, 1}));
	admit(window, 4);
	trim_window();
	require((retired == std::vector<unsigned long>{2}));
	admit(window, 5);
	trim_window();
	require((retired == std::vector<unsigned long>{2, 3}));
	require((window.order == std::vector<unsigned long>{1, 4, 5}));
	require(window.generations.size() == limit);
	require(window.generations.count(2) == 0 &&
		window.generations.count(3) == 0);

	/* Touching what is already last, or what was never there, is not
	 * a duplicate. */
	window.touch(5);
	window.touch(9);
	require((window.order == std::vector<unsigned long>{1, 4, 5, 9}));
	window.forget_place(4);
	window.forget_place(77);
	require((window.order == std::vector<unsigned long>{1, 5, 9}));
}

void a_tombstone_holds_a_place_and_no_address_space()
{
	Window window;
	for (unsigned long generation = 1; generation <= limit; ++generation)
		admit(window, generation);
	const auto ids = window.owners.size();
	/* 1 is destroyed: it lets go of its id and keeps its place. */
	retire(window, 1, true);
	require(window.generations.count(1) == 0 &&
		(window.order == std::vector<unsigned long>{1, 2, 3}));
	require(window.free_ids.size() == 1);

	/* A fourth address space takes the freed id, and the window, now four
	 * long, retires the tombstone with no address space to let go of. */
	auto &fourth = admit(window, 4);
	require(fourth.id == 1 && window.owners.size() == ids);
	const auto retired = trim(window, idle);
	require((retired == std::vector<unsigned long>{1}));
	require((window.order == std::vector<unsigned long>{2, 3, 4}));
	require(window.generations.size() == limit);
	/* Retiring the tombstone freed no id. */
	require(window.free_ids.empty());
}

/* How the window sees each address space, by number: what a cache's `use`
 * answers from the space's holders and activity. A place not named is idle. */
struct Uses final {
	std::vector<unsigned long> elsewhere;
	std::vector<unsigned long> here;
	std::size_t asked{};

	cache::PlaceUse operator()(const State &state)
	{
		++asked;
		const auto named = [&](const std::vector<unsigned long> &list) {
			return std::find(list.begin(), list.end(), state.value) !=
				list.end();
		};
		return named(elsewhere) ? cache::PlaceUse::elsewhere :
			named(here)     ? cache::PlaceUse::here :
					  cache::PlaceUse::idle;
	}
};

/* The window of `count` address spaces, 1 the oldest and `count` the one just
 * made current. */
Window window_of(unsigned long count)
{
	Window window;
	for (unsigned long generation = 1; generation <= count; ++generation)
		admit(window, generation);
	return window;
}

/* Name `count` slots in an address space's state, as stores into them do. */
void name_slots(Window &window, unsigned long generation, std::size_t count)
{
	auto &state = window.generations.at(generation);
	state.slots.resize(count);
	for (std::size_t index = 0; index < count; ++index)
		state.slots[index] = static_cast<std::uint32_t>(index);
}

using Generations = std::vector<unsigned long>;

void a_window_within_its_limit_asks_nothing()
{
	auto window = window_of(limit);
	Uses uses{{1, 2, 3}, {}};
	require(trim(window, uses).empty() && uses.asked == 0);
	require((window.order == Generations{1, 2, 3}));
}

void an_address_space_nobody_runs_goes_in_order_of_recency()
{
	auto window = window_of(limit + 1);
	/* The others run elsewhere, the oldest does not. */
	Uses uses{{2, 3, 4}, {}};
	require((trim(window, uses) == Generations{1}));
	require((window.order == Generations{2, 3, 4}));
}

void an_address_space_held_only_here_goes_in_its_turn()
{
	/* Entered lately, but nobody else holds it: the front goes as plain
	 * order would have it, whatever the others do. A lone vCPU and a
	 * private cache see every place so. */
	auto window = window_of(limit + 1);
	Uses uses{{2, 3, 4}, {1}};
	require((trim(window, uses) == Generations{1}));
	require((window.order == Generations{2, 3, 4}));

	window = window_of(limit + 1);
	uses = Uses{{}, {1, 2, 3, 4}};
	require((trim(window, uses) == Generations{1}));
}

void a_running_address_space_keeps_its_place_for_the_oldest_idle_one()
{
	auto window = window_of(limit + 1);
	/* 1 is the least recently current and runs elsewhere; 2 is the oldest
	 * place that is idle. */
	Uses uses{{1, 3}, {}};
	require((trim(window, uses) == Generations{2}));
	require((window.order == Generations{1, 3, 4}));
	require(window.generations.count(1) == 1 &&
		window.generations.count(2) == 0);

	/* 1 and 2 run elsewhere, 3 is the oldest left that is idle. */
	window = window_of(limit + 1);
	uses = Uses{{1, 2}, {}};
	require((trim(window, uses) == Generations{3}));
	require((window.order == Generations{1, 2, 4}));
}

/* The place a spared front is replaced by is one nobody wants: an address
 * space only this cache holds and entered a moment ago would be translated
 * again at once, and two of them in a window whose other places run elsewhere
 * would be, each in turn, where plain order kept both. */
void an_address_space_held_only_here_is_not_the_replacement()
{
	auto window = window_of(limit + 1);
	/* 1 and 2 run elsewhere, 3 is entered lately and held only here, and
	 * 4 is the one just made current. Of 1, 2 and 3 the fewest slots go,
	 * and 3, with the most, stays. */
	name_slots(window, 1, 5);
	name_slots(window, 2, 4);
	name_slots(window, 3, 9);
	Uses uses{{1, 2}, {3}};
	require((trim(window, uses) == Generations{2}));
	require((window.order == Generations{1, 3, 4}));

	/* The fallback is over the places that are not the newest, whatever
	 * their use: if 3 names the fewest, it goes. */
	window = window_of(limit + 1);
	name_slots(window, 1, 5);
	name_slots(window, 2, 4);
	name_slots(window, 3, 2);
	uses = Uses{{1, 2}, {3}};
	require((trim(window, uses) == Generations{3}));
	require((window.order == Generations{1, 2, 4}));

	/* An idle place after one held only here is the replacement, twice. */
	window = window_of(limit + 2);
	uses = Uses{{1}, {2}};
	require((trim(window, uses) == Generations{3, 4}));
	require((window.order == Generations{1, 2, 5}));
}

void a_tombstone_is_as_good_as_an_idle_address_space()
{
	/* The oldest of the places that are tombstones or idle goes, whichever
	 * it is. */
	auto window = window_of(limit + 1);
	retire(window, 2, true);
	Uses uses{{1}, {}};
	require((trim(window, uses) == Generations{2}));
	require((window.order == Generations{1, 3, 4}));
	/* The tombstone was not asked about: it holds no address space. */
	require(uses.asked == 1);

	window = window_of(limit + 1);
	retire(window, 3, true);
	uses = Uses{{1}, {}};
	require((trim(window, uses) == Generations{2}));
	require((window.order == Generations{1, 3, 4}));

	/* A tombstone in front goes as it always has, though what follows
	 * runs. */
	window = window_of(limit + 1);
	retire(window, 1, true);
	uses = Uses{{2, 3}, {}};
	require((trim(window, uses) == Generations{1}));
	require((window.order == Generations{2, 3, 4}));
	require(uses.asked == 0);
}

void the_one_just_made_current_is_never_the_one_to_go()
{
	/* The newest place is idle and holds nothing, and still stays. */
	auto window = window_of(limit + 1);
	for (unsigned long generation = 1; generation <= limit; ++generation)
		name_slots(window, generation, 10);
	Uses uses{{1, 2, 3}, {}};
	require((trim(window, uses) == Generations{1}));
	require(window.generations.count(limit + 1) == 1 &&
		window.order.back() == limit + 1);

	/* With room for one, the only place that can go is the other. */
	using Single = cache::GenerationWindow<unsigned long, State, 1>;
	Single single;
	for (unsigned long generation = 1; generation <= 2; ++generation) {
		auto [entry, inserted] =
			single.generations.try_emplace(generation);
		require(inserted);
		single.enroll(entry->second, generation);
		single.touch(generation);
	}
	std::vector<unsigned long> retired;
	single.trim(
		[&](unsigned long generation) {
			retired.push_back(generation);
			const auto owner = single.generations.find(generation);
			single.reserve_id();
			single.leave(owner, owner->second.id);
			single.forget_place(generation);
		},
		[](const State &) { return cache::PlaceUse::elsewhere; });
	require((retired == Generations{1}) &&
		(single.order == Generations{2}));
}

void when_no_address_space_is_idle_the_one_with_fewest_slots_goes()
{
	auto window = window_of(limit + 1);
	name_slots(window, 1, 5);
	name_slots(window, 2, 2);
	name_slots(window, 3, 9);
	name_slots(window, 4, 1);
	Uses uses{{1, 2, 3, 4}, {}};
	require((trim(window, uses) == Generations{2}));
	require((window.order == Generations{1, 3, 4}));

	/* Of equal ones the least recently current. */
	window = window_of(limit + 1);
	name_slots(window, 1, 4);
	name_slots(window, 2, 4);
	name_slots(window, 3, 9);
	uses = Uses{{1, 2, 3, 4}, {}};
	require((trim(window, uses) == Generations{1}));

	/* A list that overflowed counts as the most there is, however many
	 * entries it kept. */
	window = window_of(limit + 1);
	name_slots(window, 1, 1);
	window.generations.at(1).slots_complete = false;
	name_slots(window, 2, 7);
	name_slots(window, 3, 8);
	uses = Uses{{1, 2, 3, 4}, {}};
	require((trim(window, uses) == Generations{2}));
	window = window_of(limit + 1);
	for (unsigned long generation = 1; generation <= limit; ++generation)
		window.generations.at(generation).slots_complete = false;
	uses = Uses{{1, 2, 3, 4}, {}};
	require((trim(window, uses) == Generations{1}));
}

/* A new address space's account counts as entered for as long as its history
 * is its own, and names no slot yet, so every place of a window that has only
 * met new ones looks alike: the least recently current goes, as it always
 * has. */
void places_that_look_alike_go_in_order_of_recency()
{
	auto window = window_of(limit + 1);
	Uses uses{{1, 2, 3, 4}, {}};
	require((trim(window, uses) == Generations{1}));
	require((window.order == Generations{2, 3, 4}));
}

void a_trim_that_retires_several_asks_again_each_time()
{
	auto window = window_of(limit + 2);
	/* 1 and 2 run elsewhere; 3 and 4 are idle. Retiring 3 first leaves
	 * 1, 2, 4. */
	Uses uses{{1, 2}, {}};
	require((trim(window, uses) == Generations{3, 4}));
	require((window.order == Generations{1, 2, 5}));

	/* Every one runs elsewhere and holds slots: the fewest go first, and a
	 * place that went is not asked about again. */
	window = window_of(limit + 2);
	name_slots(window, 1, 3);
	name_slots(window, 2, 1);
	name_slots(window, 3, 2);
	name_slots(window, 4, 5);
	uses = Uses{{1, 2, 3, 4, 5}, {}};
	require((trim(window, uses) == Generations{2, 3}));
	require((window.order == Generations{1, 4, 5}));
}

void clearing_forgets_everything_but_the_id_zero_slot()
{
	Window window;
	auto &state = admit(window, 1);
	admit(window, 2);
	retire(window, 2);
	window.current = &state;
	window.bind(state);
	window.clear();
	require(window.generations.empty() && window.order.empty() &&
		window.free_ids.empty());
	require(window.owners.size() == 1 && window.owners[0] == nullptr);
	require(window.current == nullptr &&
		window.current_owner == cache::no_owner &&
		window.current_epoch == 0);
	require(admit(window, 3).id == 1);
}

} // namespace

int main()
{
	ids_count_from_one_and_are_recycled_last_in_first_out();
	the_last_id_a_tag_can_carry_is_handed_out();
	binding_names_the_owner_and_the_epoch_a_lookup_compares();
	an_epoch_wrap_clears_the_validations_it_would_repeat();
	the_order_keeps_the_least_recently_current_first();
	a_tombstone_holds_a_place_and_no_address_space();
	a_window_within_its_limit_asks_nothing();
	an_address_space_nobody_runs_goes_in_order_of_recency();
	an_address_space_held_only_here_goes_in_its_turn();
	a_running_address_space_keeps_its_place_for_the_oldest_idle_one();
	an_address_space_held_only_here_is_not_the_replacement();
	a_tombstone_is_as_good_as_an_idle_address_space();
	the_one_just_made_current_is_never_the_one_to_go();
	when_no_address_space_is_idle_the_one_with_fewest_slots_goes();
	places_that_look_alike_go_in_order_of_recency();
	a_trim_that_retires_several_asks_again_each_time();
	clearing_forgets_everything_but_the_id_zero_slot();
	return 0;
}
