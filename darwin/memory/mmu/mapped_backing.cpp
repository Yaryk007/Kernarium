// SPDX-License-Identifier: GPL-2.0-or-later
#include "mapped_backing.hpp"

#include "allocation_fault.hpp"
#include "../shared_backing.hpp"

#include "uml_darwin_guest.h"

#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <new>
#include <sys/mman.h>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace uml::memory {
namespace {

constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
std::atomic_uint64_t next_code_page_identity{1};
std::atomic_bool sharing_latched{false};

/* The `fresh` word of a record: bits 0-23 count the executable guest-code
 * mappings that name the page, bits 24-62 are the epoch, and bit 63 says the
 * record can no longer be trusted to share. The epoch is the high field, so
 * one more step past its largest value carries into bit 63 and sets it:
 * exhausting the epoch fails closed. */
constexpr std::uint64_t fresh_count_mask = (std::uint64_t{1} << 24) - 1;
constexpr std::uint64_t fresh_epoch_step = std::uint64_t{1} << 24;
constexpr std::uint64_t fresh_epoch_mask =
	((std::uint64_t{1} << 39) - 1) << 24;
constexpr std::uint64_t fresh_never_share = std::uint64_t{1} << 63;

/* Publications that name no frame: a kernel write whose target page its
 * address space does not map reaches every generation but moves no record.
 * The count rides in the high bits of every page's reported epoch, which
 * moves the epoch of every page at once. It stops at its limit, and a stamp
 * that loads it there says never-share (no verdict is shared again) before
 * the field could wrap. */
constexpr unsigned unnamed_shift = 39;
constexpr std::uint64_t unnamed_limit = std::uint64_t{1} << 24;
std::atomic_uint64_t unnamed_publications{};

} // namespace

struct MappedBacking::CodePage final
#if DARWIN_UML_X86_64_GUEST
	/* The frame accounting's fields (frame_accounting.hpp). */
	: MappedBacking::FrameRecord
#endif
{
#if DARWIN_UML_X86_64_GUEST
	explicit CodePage(DomainHeap *heap) noexcept
		: MappedBacking::FrameRecord{heap},
		  identity{next_code_page_identity.fetch_add(
			1, std::memory_order_relaxed)}
	{
	}
#else
	CodePage() noexcept
		: identity{next_code_page_identity.fetch_add(
			1, std::memory_order_relaxed)}
	{
	}
#endif

	const std::uint64_t identity;
	std::atomic_uint64_t version{1};
	std::atomic_uint64_t fresh{};
};

std::shared_ptr<MappedBacking> MappedBacking::create(
	int fd, const struct stat &status)
{
#if DARWIN_UML_X86_64_GUEST
	/* A backing made outside an address space keeps its metadata in a heap
	 * of its own. */
	return create(fd, status,
		std::make_shared<FrameAccountingDomain>(
			std::make_shared<DomainHeap>()));
}

std::shared_ptr<MappedBacking> MappedBacking::create(int fd,
	const struct stat &status, std::shared_ptr<FrameAccountingDomain> domain)
{
#endif
	if (status.st_size <= 0 ||
	    static_cast<std::uintmax_t>(status.st_size) > SIZE_MAX)
		return {};
	const auto size = static_cast<std::size_t>(status.st_size);
	auto mapping = acquire_shared_backing_view(
		fd, 0, size, PROT_READ | PROT_WRITE);
	if (!mapping)
		return {};
	UML_MEMORY_BACKING_MAPPING_CREATED();
	struct CounterGuard final {
		bool owned{true};
		~CounterGuard()
		{
			if (owned)
				UML_MEMORY_BACKING_MAPPING_DESTROYED();
		}
	} guard;
	UML_MEMORY_ALLOCATION_CHECKPOINT(
		testing::AllocationSite::backing);
#if DARWIN_UML_X86_64_GUEST
	auto *backing = new MappedBacking{std::move(mapping), size, std::move(domain)};
#else
	auto *backing = new MappedBacking{std::move(mapping), size};
#endif
	guard.owned = false;
	return std::shared_ptr<MappedBacking>{backing};
}

#if DARWIN_UML_X86_64_GUEST
MappedBacking::MappedBacking(SharedBackingView mapping, std::size_t size,
	std::shared_ptr<FrameAccountingDomain> domain) noexcept
	: domain_(std::move(domain)), mapping_(std::move(mapping)), size_(size),
	  code_pages_(0, std::hash<std::uint64_t>{}, std::equal_to<std::uint64_t>{},
		Allocator<std::pair<const std::uint64_t, RecordOwner>>{domain_->heap()}),
	  code_index_leaves_(Allocator<LeafOwner>{domain_->heap()}),
	  writer_leaves_(FrameAllocator<HeapOwned<WriterLeaf>>{domain_->heap()})
#else
MappedBacking::MappedBacking(
	SharedBackingView mapping, std::size_t size) noexcept
	: mapping_(std::move(mapping)), size_(size)
#endif
{
	const auto pages = size_ / page_size + 1U;
	const auto roots = (pages + code_index_leaf_pages - 1U) /
		code_index_leaf_pages;
	/* One pointer per leaf, four kilobytes for a gibibyte of backing. A
	 * backing that cannot afford it keeps the locked lookup. */
#if DARWIN_UML_X86_64_GUEST
	/* The frame accounting needs the index: it refuses (prepare_writers) a
	 * backing without one, so that no lookup under a stripe lock takes the
	 * records' mutex. */
	try {
		code_index_ = make_array_in_heap<std::atomic<CodeIndexLeaf *>>(
			domain_->heap(), roots);
	} catch (...) {
	}
#else
	code_index_.reset(new (std::nothrow)
		std::atomic<CodeIndexLeaf *>[roots]{});
#endif
	if (code_index_)
		code_index_pages_ = roots * code_index_leaf_pages;
	/* The same shape for the holder words: four kilobytes of roots for a
	 * gibibyte, from the ordinary allocator in every build (not the domain
	 * heap), sized by the backing and freed with it. A backing that cannot
	 * afford them is unknown to its translators and publishers alike. */
	const auto holder_roots = (pages + code_holders::leaf_frames - 1U) /
		code_holders::leaf_frames;
	holder_roots_.reset(new (std::nothrow)
		std::atomic<code_holders::Leaf *>[holder_roots]{});
	if (holder_roots_)
		holder_frames_ = holder_roots * code_holders::leaf_frames;
	else
		mark_holders_unknown();
}

MappedBacking::~MappedBacking()
{
	for (std::size_t root = 0; root < holder_frames_ / code_holders::leaf_frames;
	     ++root)
		delete holder_roots_[root].load(std::memory_order_relaxed);
	if (mapping_)
		UML_MEMORY_BACKING_MAPPING_DESTROYED();
}

code_holders::Word *MappedBacking::holder_word(std::uint64_t offset) noexcept
{
	const auto frame = offset / page_size;
	if (frame >= holder_frames_) {
		/* No root, or a frame the backing does not have: nowhere to
		 * register, so nothing can be told which translators hold it. */
		mark_holders_unknown();
		return nullptr;
	}
	auto &root = holder_roots_[frame / code_holders::leaf_frames];
	auto *leaf = root.load(std::memory_order_acquire);
	if (leaf == nullptr)
		leaf = make_holder_leaf(root);
	if (leaf == nullptr) {
		mark_holders_unknown();
		return nullptr;
	}
	return &(*leaf)[frame % code_holders::leaf_frames];
}

code_holders::Word *MappedBacking::find_holder_word(
	std::uint64_t offset) const noexcept
{
	const auto frame = offset / page_size;
	if (frame >= holder_frames_)
		return nullptr;
	auto *leaf = holder_roots_[frame / code_holders::leaf_frames].load(
		std::memory_order_acquire);
	return leaf != nullptr ? &(*leaf)[frame % code_holders::leaf_frames] :
		nullptr;
}

code_holders::Leaf *MappedBacking::make_holder_leaf(
	std::atomic<code_holders::Leaf *> &root) noexcept
{
	try {
		const std::lock_guard lock{holder_mutex_};
		if (auto *made = root.load(std::memory_order_relaxed))
			return made;
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::holder_leaf);
		auto *leaf = new (std::nothrow) code_holders::Leaf{};
		if (leaf == nullptr)
			return nullptr;
		root.store(leaf, std::memory_order_release);
		code_holders::count(&code_holders::detail::CounterStore::leaves);
		code_holders::report_counters_once();
		return leaf;
	} catch (...) {
		return nullptr;
	}
}

void MappedBacking::mark_holders_unknown() noexcept
{
	if (!holders_unknown_.exchange(true, std::memory_order_seq_cst))
		code_holders::count(
			&code_holders::detail::CounterStore::unknown_backings);
	/* The raised flag is what a publisher reads after it stores the bytes,
	 * so it goes out before this translator reads them. */
	std::atomic_thread_fence(std::memory_order_seq_cst);
}

std::byte *MappedBacking::at(std::uint64_t offset) const noexcept
{
	auto *data = static_cast<std::byte *>(mapping_.data());
	return offset < size_ ? data + static_cast<std::size_t>(offset) : nullptr;
}

void *MappedBacking::code_page_handle(std::uint64_t offset) noexcept
{
	const auto page = offset / page_size;
	/* A record is created once and never destroyed or moved, so a published
	 * index entry stays true for the backing's lifetime and the repeat ask --
	 * every revalidation of every cached translation on the page -- answers
	 * with two acquiring loads instead of a reader-writer lock whose unlock
	 * signals a condition variable. */
	if (code_index_ && page < code_index_pages_) {
		auto *leaf = code_index_[page / code_index_leaf_pages].load(
			std::memory_order_acquire);
		if (leaf != nullptr) {
			auto *found = (*leaf)[page % code_index_leaf_pages].load(
				std::memory_order_acquire);
			if (found != nullptr)
				return found;
		}
	}
	return install_code_page(page);
}

void *MappedBacking::install_code_page(std::uint64_t page) noexcept
{
	try {
		UML_MEMORY_ALLOCATION_CHECKPOINT(testing::AllocationSite::records);
		const std::lock_guard lock{code_pages_mutex_};
		auto &slot = code_pages_[page];
		if (!slot) {
			UML_MEMORY_ALLOCATION_CHECKPOINT(
				testing::AllocationSite::code_record);
#if DARWIN_UML_X86_64_GUEST
			slot = make_in_heap<CodePage>(domain_->heap(), domain_->heap());
#else
			slot = std::make_unique<CodePage>();
#endif
		}
		auto *handle = slot.get();
		if (!code_index_ || page >= code_index_pages_)
			return handle;
		auto &root = code_index_[page / code_index_leaf_pages];
		auto *leaf = root.load(std::memory_order_relaxed);
		if (leaf == nullptr) {
#if DARWIN_UML_X86_64_GUEST
			auto owned = make_in_heap<CodeIndexLeaf>(domain_->heap());
#else
			auto owned = std::make_unique<CodeIndexLeaf>();
#endif
			leaf = owned.get();
			code_index_leaves_.push_back(std::move(owned));
			root.store(leaf, std::memory_order_release);
		}
		(*leaf)[page % code_index_leaf_pages].store(
			handle, std::memory_order_release);
		return handle;
	} catch (...) {
		/* Without the index every ask takes this lock, which is the
		 * behaviour this replaces. */
		return nullptr;
	}
}

bool MappedBacking::load_code_page_stamp(
	const void *handle, RecordStamp &stamp) noexcept
{
	const auto *page = static_cast<const CodePage *>(handle);
	if (page == nullptr)
		return false;
	stamp.identity = page->identity;
	stamp.version = page->version.load(std::memory_order_acquire);
	const auto fresh = page->fresh.load(std::memory_order_acquire);
	const auto unnamed = unnamed_publications.load(std::memory_order_acquire);
	stamp.epoch = ((fresh & fresh_epoch_mask) >> 24) |
		((unnamed & (unnamed_limit - 1)) << unnamed_shift);
	/* A count at its limit would read as zero in its field: it says
	 * never-share from this very load, not from the latch, which is set
	 * after the count and which a reader can run ahead of. */
	stamp.never_share = (fresh & fresh_never_share) != 0 ||
		unnamed >= unnamed_limit;
	return true;
}

void MappedBacking::note_unnamed_publication() noexcept
{
	/* The count stops at its limit. */
	auto count = unnamed_publications.load(std::memory_order_relaxed);
	while (count < unnamed_limit &&
	       !unnamed_publications.compare_exchange_weak(count, count + 1,
		       std::memory_order_seq_cst, std::memory_order_relaxed)) {
	}
	if (count + 1 >= unnamed_limit)
		latch_sharing_off();
}

void MappedBacking::invalidate_code_page(void *handle) noexcept
{
	auto *page = static_cast<CodePage *>(handle);
	if (page == nullptr)
		return;
	page->version.fetch_add(1, std::memory_order_release);
	/* A step past the largest epoch carries into the never-share bit, which
	 * is for good: the step is made only while the bit is clear, so that no
	 * publication after the mark can carry out of the word and clear it. */
	auto fresh = page->fresh.load(std::memory_order_relaxed);
	for (;;) {
		if ((fresh & fresh_never_share) != 0)
			return;
		if (page->fresh.compare_exchange_weak(fresh,
			    fresh + fresh_epoch_step, std::memory_order_release,
			    std::memory_order_relaxed))
			return;
	}
}

void MappedBacking::retire_code_page(std::uint64_t offset) noexcept
{
	if (auto *page = static_cast<CodePage *>(find_code_page(offset));
	    page != nullptr)
		page->version.fetch_add(1, std::memory_order_release);
}

void *MappedBacking::find_code_page(std::uint64_t offset) noexcept
{
	const auto page = offset / page_size;
	if (code_index_ && page < code_index_pages_) {
		/* Every record of an indexed page is in the index. */
		const auto *leaf = code_index_[page / code_index_leaf_pages].load(
			std::memory_order_acquire);
		if (leaf == nullptr)
			return nullptr;
		return (*leaf)[page % code_index_leaf_pages].load(
			std::memory_order_acquire);
	}
	try {
		const std::lock_guard lock{code_pages_mutex_};
		const auto found = code_pages_.find(page);
		return found == code_pages_.end() ? nullptr : found->second.get();
	} catch (...) {
		return nullptr;
	}
}

void *MappedBacking::obtain_code_page(std::uint64_t offset)
{
	if (auto *handle = code_page_handle(offset); handle != nullptr)
		return handle;
	throw std::bad_alloc{};
}

void MappedBacking::executable_mapping_added(void *handle) noexcept
{
	auto *page = static_cast<CodePage *>(handle);
	auto fresh = page->fresh.load(std::memory_order_relaxed);
	for (;;) {
		if ((fresh & fresh_never_share) != 0)
			return;
		const auto next = (fresh & fresh_count_mask) == fresh_count_mask ?
			fresh | fresh_never_share : fresh + 1;
		if (page->fresh.compare_exchange_weak(fresh, next,
			    std::memory_order_acq_rel, std::memory_order_relaxed))
			return;
	}
}

void MappedBacking::executable_mapping_removed(void *handle) noexcept
{
	auto *page = static_cast<CodePage *>(handle);
	auto fresh = page->fresh.load(std::memory_order_relaxed);
	for (;;) {
		if ((fresh & fresh_never_share) != 0)
			return;
		const auto count = fresh & fresh_count_mask;
		std::uint64_t next;
		if (count > 1) {
			next = fresh - 1;
		} else {
			/* The last departure stores a count of zero and the next
			 * epoch in one step; a departure at zero is the accounting's
			 * bug, which the epoch step also contains. A step past the
			 * largest epoch carries into the never-share bit. */
			next = (fresh & ~fresh_count_mask) + fresh_epoch_step;
			if (count == 0)
				UML_MEMORY_COUNT_ANOMALY();
		}
		if (page->fresh.compare_exchange_weak(fresh, next,
			    std::memory_order_acq_rel, std::memory_order_relaxed))
			return;
	}
}

void latch_sharing_off() noexcept
{
	sharing_latched.store(true, std::memory_order_relaxed);
}

bool sharing_latched_off() noexcept
{
	return sharing_latched.load(std::memory_order_relaxed);
}

#if UML_MEMORY_ALLOCATION_TESTING
namespace testing {
void clear_sharing_latch() noexcept
{
	sharing_latched.store(false, std::memory_order_relaxed);
}

void set_unnamed_publication_count(std::uint64_t count) noexcept
{
	unnamed_publications.store(count, std::memory_order_seq_cst);
}

std::uint64_t unnamed_publication_count() noexcept
{
	return unnamed_publications.load(std::memory_order_seq_cst);
}

std::uint64_t unnamed_publication_limit() noexcept
{
	return unnamed_limit;
}
} // namespace testing
#endif

#if UML_MEMORY_ALLOCATION_TESTING || UML_MEMORY_COUNT_AUDIT
std::uint64_t MappedBacking::executable_mappings(const void *handle) noexcept
{
	return static_cast<const CodePage *>(handle)->fresh.load(
		std::memory_order_acquire) & fresh_count_mask;
}
#endif

#if UML_MEMORY_ALLOCATION_TESTING

void MappedBacking::preset_for_testing(void *handle, std::uint64_t count,
	std::uint64_t epoch) noexcept
{
	static_cast<CodePage *>(handle)->fresh.store(
		(count & fresh_count_mask) | ((epoch << 24) & fresh_epoch_mask),
		std::memory_order_release);
}

std::uint64_t MappedBacking::word_for_testing(const void *handle) noexcept
{
	return static_cast<const CodePage *>(handle)->fresh.load(
		std::memory_order_acquire);
}

void MappedBacking::preset_word_for_testing(void *handle,
	std::uint64_t word) noexcept
{
	static_cast<CodePage *>(handle)->fresh.store(word,
		std::memory_order_release);
}
#endif

} // namespace uml::memory

#if DARWIN_UML_X86_64_GUEST
namespace uml::memory {

bool MappedBacking::prepare_writers(
	std::uint64_t offset, bool record) noexcept
{
	const auto frame = offset / frame_bytes;
	const auto leaf = frame / writer_leaf_frames;
	/* A frame without a slot in the record index would be looked up under
	 * the records' mutex, which a stripe lock must never wait for. */
	if (!code_index_ || offset / page_size >= code_index_pages_)
		return false;
	try {
		if (record && code_page_handle(offset) == nullptr)
			return false;
		const std::lock_guard lock{code_pages_mutex_};
		auto *index = writer_index_.load(std::memory_order_relaxed);
		if (index == nullptr) {
			const auto leaves =
				(size_ / frame_bytes + 1U + writer_leaf_frames - 1U) /
				writer_leaf_frames;
			auto storage = make_array_in_heap<std::atomic<WriterLeaf *>>(
				domain_->heap(), leaves);
			writer_index_leaves_ = leaves;
			index = storage.get();
			writer_index_storage_ = std::move(storage);
			writer_index_.store(index, std::memory_order_release);
		}
		if (leaf >= writer_index_leaves_)
			return false;
		if (index[leaf].load(std::memory_order_relaxed) == nullptr) {
			auto owned = make_in_heap<WriterLeaf>(domain_->heap());
			owned->fill(0);
			auto *words = owned.get();
			writer_leaves_.push_back(std::move(owned));
			index[leaf].store(words, std::memory_order_release);
		}
		return true;
	} catch (...) {
		return false;
	}
}

std::uint64_t *MappedBacking::writer_word(std::uint64_t offset) noexcept
{
	const auto frame = offset / frame_bytes;
	const auto leaf = frame / writer_leaf_frames;
	/* The index is published before any leaf and a leaf before any word. */
	auto *index = writer_index_.load(std::memory_order_acquire);
	if (index == nullptr || leaf >= writer_index_leaves_)
		return nullptr;
	auto *words = index[leaf].load(std::memory_order_acquire);
	return words == nullptr ? nullptr : &(*words)[frame % writer_leaf_frames];
}

MappedBacking::FrameRecord *MappedBacking::frame_record(
	std::uint64_t offset) noexcept
{
	/* A frame without a slot in the record index would later be looked up
	 * under the records' mutex: it has no record (see prepare_writers). */
	if (!code_index_ || offset / page_size >= code_index_pages_)
		return nullptr;
	return static_cast<CodePage *>(code_page_handle(offset));
}

MappedBacking::FrameRecord *MappedBacking::find_frame_record(
	std::uint64_t offset) noexcept
{
	const auto page = offset / page_size;
	if (!code_index_ || page >= code_index_pages_)
		return nullptr;
	/* Every record of an indexed page is in the index. */
	const auto *leaf = code_index_[page / code_index_leaf_pages].load(
		std::memory_order_acquire);
	if (leaf == nullptr)
		return nullptr;
	return (*leaf)[page % code_index_leaf_pages].load(
		std::memory_order_acquire);
}

const MappedBacking::FrameRecord *MappedBacking::record_of(
	const void *handle) noexcept
{
	return static_cast<const CodePage *>(handle);
}

std::uint64_t MappedBacking::record_identity(const FrameRecord &record) noexcept
{
	return static_cast<const CodePage &>(record).identity;
}

void MappedBacking::move_stamp(FrameRecord &record) noexcept
{
	invalidate_code_page(&static_cast<CodePage &>(record));
}

} // namespace uml::memory
#endif
