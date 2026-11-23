// SPDX-License-Identifier: GPL-2.0-or-later
#include "code_page_registry.hpp"
#include "memory/domain_heap.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

namespace uml::cpu::x86_64 {
namespace {

/* Never destroyed, so a registry released during process teardown still
 * finds its entry and its lock. */
struct Table final {
	std::mutex mutex;
	std::unordered_map<std::uint64_t, std::weak_ptr<CodePageRegistry>>
		entries;
};
Table &table()
{
	static auto *instance = new Table{};
	return *instance;
}

} // namespace

std::shared_ptr<CodePageRegistry> CodePageRegistry::find(
	std::uint64_t generation)
{
	auto &shared = table();
	const std::lock_guard lock{shared.mutex};
	const auto entry = shared.entries.find(generation);
	return entry == shared.entries.end() ? nullptr : entry->second.lock();
}

std::shared_ptr<CodePageRegistry> CodePageRegistry::acquire(
	std::uint64_t generation)
{
	return acquire(generation, WaitPolicy{});
}

std::shared_ptr<CodePageRegistry> CodePageRegistry::acquire(
	std::uint64_t generation, const WaitPolicy &policy)
{
	auto &shared = table();
	const std::lock_guard lock{shared.mutex};
	auto [entry, inserted] = shared.entries.try_emplace(generation);
	if (!inserted)
		if (auto existing = entry->second.lock())
			return existing;
	auto created = std::make_shared<CodePageRegistry>(policy);
	created->generation_ = generation;
	entry->second = created;
	return created;
}

CodePageRegistry::CodePageRegistry() : CodePageRegistry(WaitPolicy{}) {}

CodePageRegistry::CodePageRegistry(const WaitPolicy &policy)
	: policy_{policy},
	  bits_{std::make_unique<std::atomic<std::uint64_t>[]>(bit_count / 64U)},
	  counts_{std::make_unique<std::uint8_t[]>(bit_count)}
{
	for (std::size_t word = 0; word < bit_count / 64U; ++word)
		bits_[word].store(0, std::memory_order_relaxed);
	std::fill_n(counts_.get(), bit_count, std::uint8_t{0});
}

/* Generations are never reused, so the entry of a released registry would
 * otherwise stay behind for the life of the process. A registry acquired for
 * the same generation after this one expired owns the entry now and keeps it. */
CodePageRegistry::~CodePageRegistry()
{
	if (!generation_)
		return;
	auto &shared = table();
	const std::lock_guard lock{shared.mutex};
	const auto entry = shared.entries.find(*generation_);
	if (entry != shared.entries.end() && entry->second.expired())
		shared.entries.erase(entry);
}

void CodePageRegistry::join(Participant &participant)
{
	const std::lock_guard lock{participants_mutex_};
	if (std::find_if(participants_.begin(), participants_.end(),
		    [&](const Member &member) {
			    return member.participant == &participant;
		    }) == participants_.end())
		participants_.push_back({&participant, next_serial_++});
}

void CodePageRegistry::join(Participant &participant,
	RetireWriteTranslation retire, void *context)
{
	const std::lock_guard lock{participants_mutex_};
	if (std::find_if(participants_.begin(), participants_.end(),
		    [&](const Member &member) {
			    return member.participant == &participant;
		    }) != participants_.end())
		return;
	const auto serial = next_serial_;
	retirers_.push_back({&participant, serial, retire, context});
	try {
		participants_.push_back({&participant, serial});
	} catch (...) {
		retirers_.pop_back();
		throw;
	}
	++next_serial_;
}

void CodePageRegistry::leave(Participant &participant) noexcept
{
	/* A departure must follow the participant's last leave of the engine,
	 * in its own thread or after it: a waiter treats a departed member as
	 * having left the engine, and every store the member translated must
	 * have landed by then. A counter, not an assertion, says whether any
	 * caller breaks that: it must stay 0. */
	if ((participant.engine_epoch.load(std::memory_order_acquire) & 1U) != 0)
		leaves_inside_engine_.fetch_add(1, std::memory_order_relaxed);
	const std::lock_guard lock{participants_mutex_};
	const auto found = std::find_if(participants_.begin(),
		participants_.end(), [&](const Member &member) {
			return member.participant == &participant;
		});
	if (found == participants_.end())
		return;
	const auto serial = found->serial;
	participants_.erase(found);
	std::erase_if(retirers_, [&](const Retirer &retirer) {
		return retirer.serial == serial;
	});
	std::erase_if(expiries_, [&](const StuckMember &stuck) {
		return stuck.serial == serial;
	});
}

void CodePageRegistry::retire_write_translations(std::uint64_t page) noexcept
{
	const auto index = native_tlb_index(page);
	const std::shared_lock lock{participants_mutex_};
	for (const auto &member : participants_) {
		if (member.participant->write_translations == nullptr)
			continue;
		std::atomic_ref<std::uint64_t>{
			member.participant->write_translations[index].write_page}
			.store(1, std::memory_order_relaxed);
	}
	for (const auto &retirer : retirers_)
		retirer.retire(retirer.context, page);
}

bool CodePageRegistry::holds_exactly(
	std::uint64_t address, std::size_t bit) const noexcept
{
	if (inexact_.load(std::memory_order_relaxed))
		return true;
	const auto &stripe = stripe_for(bit);
	const std::shared_lock lock{stripe.mutex};
	const auto found = stripe.pages.find(address & ~std::uint64_t{0xfff});
	return found != stripe.pages.end() && found->second.count != 0;
}

CodePageRegistry::Settlement CodePageRegistry::add(
	std::uint64_t page, const Participant *self) noexcept
{
	page &= ~std::uint64_t{0xfff};
	const auto bit = bit_index(page);
	auto &stripe = stripe_for(bit);
	std::unique_lock lock{stripe.mutex};
	bool gained = false;
	/* The gain this call settles, 0 when the page cannot be tracked. */
	std::uint64_t gain = 0;
	bool unsettled = false;
	try {
		auto &entry = stripe.pages.try_emplace(page).first->second;
		gained = entry.count++ == 0;
		if (gained)
			entry.gain = ++stripe.gains;
		gain = entry.gain;
		unsettled = entry.settled != entry.gain;
	} catch (...) {
		/* Without its count the page is held for as long as its bit is. */
		inexact_.store(true, std::memory_order_relaxed);
		gained = true;
	}
	auto &count = counts_[bit];
	if (count != 0xff && count++ == 0)
		bits_[bit >> 6U].fetch_or(std::uint64_t{1} << (bit & 63U),
			std::memory_order_release);
	/* A call that finds the gain settled has nothing to wait for. A call
	 * that finds it unsettled is a second translator of the page: the first
	 * may still be waiting, and the second must not read the page before the
	 * stores in flight have landed either. */
	if (!gained && !unsettled)
		return {};
	if (gained)
		changes_.fetch_add(1, std::memory_order_acq_rel);
	/* Order the count against the retirement below: a miss handler that read
	 * the old count before this fence sees the new count after its own. */
	std::atomic_thread_fence(std::memory_order_seq_cst);
	retire_write_translations(page);
	/* Participants may be inside the engine with a store already translated
	 * through the entry just retired. Wait for each to leave once so that
	 * store lands before the translation reads the page. */
	lock.unlock();
	auto settlement = wait_for_participants(self);
	if (settlement.settled && gain != 0) {
		const std::lock_guard relock{stripe.mutex};
		if (const auto found = stripe.pages.find(page);
		    found != stripe.pages.end() && found->second.settled < gain)
			found->second.settled = gain;
	}
	return settlement;
}

CodePageRegistry::Settlement CodePageRegistry::retire_and_wait(
	std::uint64_t page, const Participant *self) noexcept
{
	page &= ~std::uint64_t{0xfff};
	/* Order the registration that owes this against the retirement: a fill
	 * that read the frame before it sees the retired entry or is waited for. */
	std::atomic_thread_fence(std::memory_order_seq_cst);
	retire_write_translations(page);
	return wait_for_participants(self);
}

CodePageRegistry::Settlement CodePageRegistry::retire_all_and_wait(
	const Participant *self) noexcept
{
	std::atomic_thread_fence(std::memory_order_seq_cst);
	{
		const std::shared_lock lock{participants_mutex_};
		for (const auto &member : participants_) {
			if (member.participant->write_translations == nullptr)
				continue;
			for (std::size_t index = 0; index < native_tlb_entry_count; ++index)
				std::atomic_ref<std::uint64_t>{
					member.participant->write_translations[index].write_page}
					.store(1, std::memory_order_relaxed);
		}
		for (const auto &retirer : retirers_)
			retirer.retire(retirer.context, all_pages);
	}
	return wait_for_participants(self);
}

std::vector<std::shared_ptr<CodePageRegistry>> CodePageRegistry::registries()
{
	auto &shared = table();
	std::vector<std::shared_ptr<CodePageRegistry>> found;
	const std::lock_guard lock{shared.mutex};
	found.reserve(shared.entries.size());
	for (const auto &[generation, entry] : shared.entries) {
		(void)generation;
		if (auto registry = entry.lock())
			found.push_back(std::move(registry));
	}
	return found;
}

CodePageRegistry::Settlement CodePageRegistry::wait_for_participants(
	const Participant *self) noexcept
{
	/* A participant is read only while the lock is held and the pair named
	 * here is still a member: `leave()` waits for nobody, and the owner
	 * frees a participant once it has left. */
	struct Awaited final {
		Participant *participant;
		std::uint64_t serial;
		std::uint64_t epoch;
	};
	Settlement result;
	std::vector<Awaited> waiting;
	bool stale_expiry = false;
	{
		const std::shared_lock lock{participants_mutex_};
		for (const auto &member : participants_) {
			if (member.participant == self)
				continue;
			const auto epoch = member.participant->engine_epoch.load(
				std::memory_order_acquire);
			if ((epoch & 1U) == 0)
				continue;
			const bool expired = std::any_of(expiries_.begin(),
				expiries_.end(), [&](const StuckMember &stuck) {
					return stuck.serial == member.serial &&
						stuck.epoch == epoch;
				});
			if (expired) {
				/* Waited out already, and it has not left since. */
				result.stuck.push_back({member.serial, epoch});
				result.settled = false;
			} else {
				waiting.push_back({member.participant, member.serial, epoch});
			}
		}
		for (const auto &stuck : expiries_) {
			const auto member = std::find_if(participants_.begin(),
				participants_.end(), [&](const Member &candidate) {
					return candidate.serial == stuck.serial;
				});
			if (member == participants_.end() ||
			    member->participant->engine_epoch.load(
				    std::memory_order_acquire) != stuck.epoch)
				stale_expiry = true;
		}
	}
	if (stale_expiry)
		forget_departed_expiries();
	if (waiting.empty())
		return result;
	const auto deadline = std::chrono::steady_clock::now() +
		policy_.slice * policy_.slices;
	/* One pass over the participants, each polled while it is a member at
	 * its recorded epoch. */
	const auto still_inside = [this](const Awaited &awaited) noexcept {
		const std::shared_lock lock{participants_mutex_};
		const auto member = std::find_if(participants_.begin(),
			participants_.end(), [&](const Member &candidate) {
				return candidate.participant == awaited.participant &&
					candidate.serial == awaited.serial;
			});
		/* Gone: it left the engine before it left the registry. */
		return member != participants_.end() &&
			member->participant->engine_epoch.load(
				std::memory_order_acquire) == awaited.epoch;
	};
	std::size_t next = 0;
	bool expired = false;
	while (next < waiting.size() && !expired) {
		if (!still_inside(waiting[next])) {
			++next;
		} else if (std::chrono::steady_clock::now() >= deadline) {
			expired = true;
		} else {
			std::this_thread::yield();
		}
	}
	if (!expired)
		return result;
	/* The deadline passed with a participant still inside the engine. Every
	 * one not yet seen leaving is stuck; remember each, say so once, and let
	 * the caller translate without caching. Another waiter may have recorded
	 * the same participant at the same epoch meanwhile, or the participant
	 * may have left, so each is checked again under the exclusive lock. */
	for (; next < waiting.size(); ++next) {
		const auto &awaited = waiting[next];
		bool stuck = false;
		bool recorded = false;
		{
			const std::lock_guard lock{participants_mutex_};
			const auto member = std::find_if(participants_.begin(),
				participants_.end(), [&](const Member &candidate) {
					return candidate.participant == awaited.participant &&
						candidate.serial == awaited.serial;
				});
			stuck = member != participants_.end() &&
				member->participant->engine_epoch.load(
					std::memory_order_acquire) == awaited.epoch;
			if (stuck && std::none_of(expiries_.begin(), expiries_.end(),
				    [&](const StuckMember &known) {
					    return known.serial == awaited.serial &&
						    known.epoch == awaited.epoch;
				    })) {
				try {
					expiries_.push_back({awaited.serial, awaited.epoch});
					recorded = true;
				} catch (...) {
					/* The next registration waits for it again. */
				}
			}
		}
		if (!stuck)
			continue;
		result.stuck.push_back({awaited.serial, awaited.epoch});
		if (recorded)
			std::fprintf(stderr, "Darwin x86 code pages: a vCPU stayed inside "
				"the engine for %lld ms (epoch %llu); code on pages it may "
				"write is not cached until it leaves\n",
				static_cast<long long>((policy_.slice * policy_.slices).count()),
				static_cast<unsigned long long>(awaited.epoch));
	}
	result.settled = result.stuck.empty();
	return result;
}

bool CodePageRegistry::left_or_moved(
	std::uint64_t serial, std::uint64_t epoch) const noexcept
{
	const std::shared_lock lock{participants_mutex_};
	const auto member = std::find_if(participants_.begin(),
		participants_.end(), [&](const Member &candidate) {
			return candidate.serial == serial;
		});
	return member == participants_.end() ||
		member->participant->engine_epoch.load(
			std::memory_order_acquire) != epoch;
}

std::size_t CodePageRegistry::expiry_records() const noexcept
{
	const std::shared_lock lock{participants_mutex_};
	return expiries_.size();
}

void CodePageRegistry::forget_departed_expiries() noexcept
{
	const std::lock_guard lock{participants_mutex_};
	std::erase_if(expiries_, [&](const StuckMember &stuck) {
		const auto member = std::find_if(participants_.begin(),
			participants_.end(), [&](const Member &candidate) {
				return candidate.serial == stuck.serial;
			});
		return member == participants_.end() ||
			member->participant->engine_epoch.load(
				std::memory_order_acquire) != stuck.epoch;
	});
}

void CodePageRegistry::remove(std::uint64_t page) noexcept
{
	page &= ~std::uint64_t{0xfff};
	const auto bit = bit_index(page);
	auto &stripe = stripe_for(bit);
	const std::lock_guard lock{stripe.mutex};
	bool lost = false;
	if (const auto found = stripe.pages.find(page);
	    found != stripe.pages.end() && found->second.count != 0)
		lost = --found->second.count == 0;
	auto &count = counts_[bit];
	if (count != 0 && count != 0xff && --count == 0)
		bits_[bit >> 6U].fetch_and(~(std::uint64_t{1} << (bit & 63U)),
			std::memory_order_release);
	if (!lost)
		return;
	/* A write translation tagged while the page held code would keep
	 * publishing; retire it so the next store refills it untagged. A
	 * translation retired under a store in flight only publishes once more. */
	retire_write_translations(page);
}

namespace {

std::atomic_uint64_t total_registrations{};
std::atomic_uint64_t total_failures{};
std::atomic_uint64_t total_refusals{};
std::atomic_uint64_t total_unsettled{};
std::atomic_uint64_t total_flush_serial_misses{};

FrameRegistrationOutcome register_frame_uncounted(
	::uml::memory::GuestAddressSpace &memory, void *execution_context,
	const CodePageRegistry::Participant *self, std::uint64_t page) noexcept
{
	FrameRegistrationOutcome outcome;
	std::array<::uml::memory::FrameDebt, 16> small{};
	/* The larger buffer is a temporary of this call, so it comes from the
	 * thread's scratch arena, which this scope resets. */
	const ::uml::memory::ScratchScope scratch;
	std::vector<::uml::memory::FrameDebt,
		::uml::memory::ScratchAllocator<::uml::memory::FrameDebt>>
		large{::uml::memory::ScratchAllocator<::uml::memory::FrameDebt>{
			scratch.arena()}};
	::uml::memory::FrameDebt *debts = small.data();
	std::size_t capacity = small.size();
	bool global_ok = false;
	::uml::memory::FrameHold hold;
	/* A debt larger than the buffer is asked for again with room for it. The
	 * writers may grow between the asks, so the asks are bounded; when the
	 * room cannot be had, or keeps being too small, the registration takes
	 * the global fallback, which needs none. */
	for (unsigned attempt = 0;; ++attempt) {
		hold = memory.hold_code_frame(
			page, execution_context, debts, capacity, global_ok);
		if (hold.result != ::uml::memory::FrameHoldResult::crowded)
			break;
		try {
			if (attempt >= 4)
				throw std::bad_alloc{};
			large.resize(hold.owed + hold.owed / 4U + 8U);
			debts = large.data();
			capacity = large.size();
		} catch (...) {
			debts = small.data();
			capacity = 0;
			global_ok = true;
		}
	}
	switch (hold.result) {
	case ::uml::memory::FrameHoldResult::settled:
		outcome.status = FrameRegistration::settled;
		outcome.record = hold.record;
		outcome.alias_changes = hold.alias_changes;
		return outcome;
	case ::uml::memory::FrameHoldResult::refused:
		outcome.status = FrameRegistration::refused;
		return outcome;
	case ::uml::memory::FrameHoldResult::failed:
	case ::uml::memory::FrameHoldResult::crowded:
		outcome.status = FrameRegistration::failed;
		return outcome;
	case ::uml::memory::FrameHoldResult::owed:
	case ::uml::memory::FrameHoldResult::global:
		break;
	}
	bool complete = true;
	const auto stuck_at = [&](std::shared_ptr<CodePageRegistry> registry,
				 CodePageRegistry::Settlement settlement) {
		complete = false;
		try {
			outcome.stuck.push_back({std::move(registry), std::move(settlement)});
		} catch (...) {
			outcome.stuck_lost = true;
		}
	};
	if (hold.result == ::uml::memory::FrameHoldResult::global) {
		/* Every participant of every generation: the writers are too many
		 * to name or not known. */
		std::vector<std::shared_ptr<CodePageRegistry>> all;
		try {
			all = CodePageRegistry::registries();
		} catch (...) {
			/* The holder stands and the debt stays owed. */
			outcome.stuck_lost = true;
			outcome.status = FrameRegistration::unsettled;
			return outcome;
		}
		for (auto &registry : all) {
			auto settlement = registry->retire_all_and_wait(self);
			if (!settlement.settled)
				stuck_at(registry, std::move(settlement));
		}
	} else {
		for (std::size_t index = 0; index < std::min(hold.owed, capacity);
		     ++index) {
			/* A generation no executor holds a registry for has no write
			 * translation to retire. */
			auto registry = CodePageRegistry::find(debts[index].generation);
			if (!registry)
				continue;
			auto settlement = registry->retire_and_wait(debts[index].page, self);
			if (settlement.settled)
				continue;
			stuck_at(std::move(registry), std::move(settlement));
		}
	}
	if (!complete) {
		outcome.status = FrameRegistration::unsettled;
		return outcome;
	}
	memory.clear_code_debt(page, execution_context, hold);
	outcome.status = FrameRegistration::settled;
	/* A global answer names no frame to remember: the frame's writers are not
	 * known or too many to name, and a registration of it asks for the same
	 * retirement again. */
	if (hold.result != ::uml::memory::FrameHoldResult::global) {
		outcome.record = hold.record;
		outcome.alias_changes = hold.alias_changes;
	}
	return outcome;
}

} // namespace

FrameRegistrationOutcome register_frame(
	::uml::memory::GuestAddressSpace &memory, void *execution_context,
	const CodePageRegistry::Participant *self, std::uint64_t page) noexcept
{
	auto outcome = register_frame_uncounted(memory, execution_context, self,
		page);
	total_registrations.fetch_add(1, std::memory_order_relaxed);
	switch (outcome.status) {
	case FrameRegistration::settled:
		break;
	case FrameRegistration::unsettled:
		total_unsettled.fetch_add(1, std::memory_order_relaxed);
		break;
	case FrameRegistration::failed:
		total_failures.fetch_add(1, std::memory_order_relaxed);
		break;
	case FrameRegistration::refused:
		total_refusals.fetch_add(1, std::memory_order_relaxed);
		break;
	}
	return outcome;
}

RegistrationTotals registration_totals() noexcept
{
	return {total_registrations.load(std::memory_order_relaxed),
		total_failures.load(std::memory_order_relaxed),
		total_refusals.load(std::memory_order_relaxed),
		total_unsettled.load(std::memory_order_relaxed),
		total_flush_serial_misses.load(std::memory_order_relaxed)};
}

void note_flush_serial_miss() noexcept
{
	total_flush_serial_misses.fetch_add(1, std::memory_order_relaxed);
}

} // namespace uml::cpu::x86_64
