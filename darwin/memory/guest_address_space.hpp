// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::memory {

enum class MemoryAccess {
	read,
	write,
	execute,
};

struct MemoryFault final {
	std::uint64_t address{};
	MemoryAccess access{MemoryAccess::read};
};

/* A stable name for the bytes of a code page: the same page at the same
 * version. The translation caches rely on it, and it is part of the layout of
 * what they keep, so it stays two words. */
struct CodePageStamp final {
	std::uint64_t identity{};
	std::uint64_t version{};

	friend bool operator==(const CodePageStamp &, const CodePageStamp &) =
		default;
};

/* What a code page says about itself, for a consumer that keeps verdicts
 * about the page's bytes across address spaces. `epoch` moves at a
 * publication that names the page's frame and when the last executable
 * mapping of the page departs, and, for every page at once, at a kernel write
 * whose target page its address space does not map; nowhere else. `immutable` is the kernel's fact for the mapping the stamp was
 * taken through: no one can change the page's bytes while it holds. A page
 * without it can be rewritten through another mapping of the frame with no
 * word to the address space that checked it, so a consumer that keeps a
 * verdict about the bytes in a mapping must require it. `shareable` is true
 * when `immutable` holds and the page's record counts its executable mappings
 * exactly. A verdict is kept across address spaces only for a shareable page,
 * under (identity, epoch). */
struct ExtendedCodePageStamp final {
	CodePageStamp stamp{};
	std::uint64_t epoch{};
	bool shareable{};
	bool immutable{};

	friend bool operator==(const ExtendedCodePageStamp &,
			       const ExtendedCodePageStamp &) = default;
};

#if DARWIN_UML_X86_64_GUEST
/* What an x86 translator asks of the memory it reads (the x86-64 guest's frame
 * accounting, darwin/memory/mmu/frame_accounting.hpp). Every method has a
 * default that says "nothing to account": a memory that cannot name a frame
 * has no aliases to publish at. */
struct FrameDebt final {
	std::uint64_t generation{};
	std::uint64_t page{};
};
enum class FrameHoldResult : std::uint8_t {
	settled,
	owed,
	refused,
	failed,
	/* Nothing changed: the debt is `owed` locations, more than the caller's
	 * buffer holds. */
	crowded,
	/* The holder stands, and the debt is every write translation of every
	 * participant. */
	global,
};
struct FrameHold final {
	FrameHoldResult result{FrameHoldResult::settled};
	std::uint64_t record_identity{};
	std::uint64_t debt_sequence{};
	std::size_t owed{};
	/* The frame's record and its holder change count as the hold left them;
	 * null when nothing stands (see frame_hold_unchanged). */
	const void *record{};
	std::uint64_t alias_changes{};
};
/* What the write fill read of the frame a page maps. */
struct FrameFill final {
	const void *backing{};
	std::uint64_t offset{};
	const void *record{};
	std::uint64_t alias_changes{};
	bool other_holder{};
};
struct FramePublication final {
	bool overflow{};
	std::size_t own_count{};
	std::uint64_t own_records{};
};
#endif

class GuestAddressSpace {
public:
	virtual ~GuestAddressSpace() = default;
	[[nodiscard]] virtual bool read(std::uint64_t address,
					std::span<std::byte> destination) = 0;
	[[nodiscard]] virtual bool write(std::uint64_t address,
					 std::span<const std::byte> source) = 0;
	/* Validate an access without changing guest memory. Emulators with
	 * protection-aware mappings override this so instructions that validate
	 * a whole transfer before writing preserve architectural fault
	 * priority. */
	[[nodiscard]] virtual bool validate_access(
		std::uint64_t, std::size_t, MemoryAccess)
	{
		return true;
	}
	[[nodiscard]] virtual bool fetch(std::uint64_t address,
					 std::span<std::byte> destination)
	{
		return read(address, destination);
	}
	[[nodiscard]] virtual std::byte *atomic_pointer(
		std::uint64_t, std::size_t, MemoryAccess)
	{
		return nullptr;
	}
	[[nodiscard]] virtual std::byte *reservation_pointer(
		std::uint64_t address, std::size_t size, MemoryAccess access)
	{
		return atomic_pointer(address, size, access);
	}
	[[nodiscard]] virtual bool is_executable(std::uint64_t) noexcept
	{
		return false;
	}
	/* A stable stamp lets a translated-code cache reject unrelated global code
	 * publications without rereading its source bytes. Implementations which
	 * cannot retain page identity return false and keep exact byte validation.
	 */
	[[nodiscard]] virtual bool code_page_stamp(
		std::uint64_t, void *, CodePageStamp &) noexcept
	{
		return false;
	}
	/* The same page's stamp with its epoch and whether it may be shared. */
	[[nodiscard]] virtual bool extended_code_page_stamp(
		std::uint64_t, void *, ExtendedCodePageStamp &) noexcept
	{
		return false;
	}
	virtual void invalidate_code(std::uint64_t) {}
	/* A translated executor may carry an already-entered memory execution
	 * context. Static memory uses it to publish directly to the owning
	 * generation; other memory implementations retain the ordinary hook.
	 */
	virtual void invalidate_code(std::uint64_t address, void *)
	{
		invalidate_code(address);
	}
#if DARWIN_UML_X86_64_GUEST
	/* Registers the page as a holder of its frame before the caller reads a
	 * byte of it, and names the writer locations whose write translations
	 * the caller must retire and wait out (`owed` entries of `debts`). A debt
	 * larger than `capacity` answers `crowded` with its size, or, when
	 * `global_ok`, registers the holder and answers `global`. */
	[[nodiscard]] virtual FrameHold hold_code_frame(
		std::uint64_t, void *, FrameDebt *, std::size_t,
		bool) noexcept
	{
		return {};
	}
	/* A completed retirement of a hold's debts covers them. */
	virtual void clear_code_debt(
		std::uint64_t, void *, const FrameHold &) noexcept
	{
	}
	/* Whether no holder has been added to the frame `hold` registered a page
	 * of since the hold, so that registering the same page again, with the
	 * mapping and the flush serial as they were, would add nothing and owe
	 * nothing. Lock-free; for a caller that is entered in the address space.
	 * A memory that cannot say answers false. */
	[[nodiscard]] virtual bool frame_hold_unchanged(
		const FrameHold &) noexcept
	{
		return false;
	}
	/* The write fill's lock-free read of the frame; false when the memory
	 * cannot say (the fill then tags from the registry alone). */
	[[nodiscard]] virtual bool frame_fill(
		std::uint64_t, void *, FrameFill &) noexcept
	{
		return false;
	}
	/* Whether what `frame_fill` read has not moved. */
	[[nodiscard]] virtual bool frame_fill_unchanged(
		const FrameFill &) noexcept
	{
		return true;
	}
	/* Publishes the store of `length` bytes at `address`, after its bytes
	 * were written, at every holder of the frame; true if it did. False means
	 * the frame has no holder but the writer's own page, and the caller
	 * publishes as it always did. `own` receives the writer's own
	 * generation's holder addresses. */
	[[nodiscard]] virtual bool publish_frame_store(std::uint64_t, std::uint64_t,
		void *, std::uint64_t *, std::size_t, FramePublication &) noexcept
	{
		return false;
	}
	/* Before a store: the holder addresses of its frame in the writer's own
	 * generation that publish_frame_store would publish at, with nothing
	 * published. Same results. */
	[[nodiscard]] virtual bool frame_own_holders(std::uint64_t, std::uint64_t,
		void *, std::uint64_t *, std::size_t, FramePublication &) noexcept
	{
		return false;
	}
	/* The generation's flush serial (frame_accounting.hpp), 0 when the
	 * memory has none. */
	[[nodiscard]] virtual std::uint64_t flush_serial(void *) noexcept
	{
		return 0;
	}
#endif
	[[nodiscard]] virtual MemoryFault last_fault() const noexcept
	{
		return {};
	}
	[[nodiscard]] virtual bool has_fault() const noexcept
	{
		return false;
	}
};

} // namespace uml::memory
