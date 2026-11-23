// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "uml_darwin_guest.h"

#if DARWIN_UML_X86_64_GUEST

#include "../domain_heap.hpp"
#include "allocation_fault.hpp"

#include <cstddef>
#include <memory>
#include <new>
#include <utility>

namespace uml::memory {

/* Storage for the x86-64 frame accounting's metadata: the writer index, the
 * code-page records and the lists they keep. It comes from the heap of the
 * address space that owns the backing and goes back with it, as the domain-heap
 * contract (docs/architecture.md) has it, so none of it enters the default
 * malloc zone. A null heap is the process's own allocator, for a backing made
 * outside an address space.
 *
 * Every allocation reports to the test variant's probe, which counts the ones
 * made while a frame's stripe lock is held; there must be none. */
[[nodiscard]] inline void *frame_allocate(
	DomainHeap *heap, std::size_t bytes, std::size_t alignment)
{
	UML_MEMORY_ALLOCATOR_PROBE();
	void *block = heap != nullptr ? heap->allocate(bytes, alignment) :
		::operator new(bytes, std::align_val_t{alignment});
	UML_MEMORY_FRAME_BYTES(static_cast<std::ptrdiff_t>(bytes));
	return block;
}

inline void frame_deallocate(DomainHeap *heap, void *block, std::size_t bytes,
	std::size_t alignment) noexcept
{
	UML_MEMORY_FRAME_BYTES(-static_cast<std::ptrdiff_t>(bytes));
	if (heap != nullptr)
		heap->deallocate(block, bytes, alignment);
	else
		::operator delete(block, std::align_val_t{alignment});
}

template <typename T>
class FrameAllocator final {
public:
	using value_type = T;
	using propagate_on_container_copy_assignment = std::true_type;
	using propagate_on_container_move_assignment = std::true_type;
	using propagate_on_container_swap = std::true_type;
	using is_always_equal = std::false_type;

	FrameAllocator() noexcept = default;
	explicit FrameAllocator(DomainHeap *heap) noexcept : heap_{heap} {}
	template <typename U>
	FrameAllocator(const FrameAllocator<U> &other) noexcept
		: heap_{other.heap()}
	{
	}

	[[nodiscard]] T *allocate(std::size_t count)
	{
		if (count > static_cast<std::size_t>(-1) / sizeof(T))
			throw std::bad_alloc{};
		return static_cast<T *>(
			frame_allocate(heap_, count * sizeof(T), alignof(T)));
	}
	void deallocate(T *block, std::size_t count) noexcept
	{
		frame_deallocate(heap_, block, count * sizeof(T), alignof(T));
	}
	[[nodiscard]] DomainHeap *heap() const noexcept { return heap_; }

	template <typename U>
	[[nodiscard]] bool operator==(const FrameAllocator<U> &other) const noexcept
	{
		return heap_ == other.heap();
	}

private:
	DomainHeap *heap_{};
};

/* Gives an object back to the heap that made it. */
template <typename T>
struct HeapDelete final {
	DomainHeap *heap{};
	void operator()(T *object) const noexcept
	{
		if (object == nullptr)
			return;
		object->~T();
		frame_deallocate(heap, object, sizeof(T), alignof(T));
	}
};

/* The same for an array of trivially destructible elements. */
template <typename T>
struct HeapArrayDelete final {
	DomainHeap *heap{};
	std::size_t count{};
	void operator()(T *elements) const noexcept
	{
		if (elements != nullptr)
			frame_deallocate(heap, elements, count * sizeof(T), alignof(T));
	}
};

template <typename T>
using HeapOwned = std::unique_ptr<T, HeapDelete<T>>;
template <typename T>
using HeapArray = std::unique_ptr<T[], HeapArrayDelete<T>>;

/* Throws std::bad_alloc, with nothing allocated. */
template <typename T, typename... Arguments>
[[nodiscard]] HeapOwned<T> make_in_heap(
	DomainHeap *heap, Arguments &&...arguments)
{
	void *block = frame_allocate(heap, sizeof(T), alignof(T));
	try {
		return HeapOwned<T>{new (block) T(std::forward<Arguments>(arguments)...),
			HeapDelete<T>{heap}};
	} catch (...) {
		frame_deallocate(heap, block, sizeof(T), alignof(T));
		throw;
	}
}

/* `count` value-initialized elements. */
template <typename T>
[[nodiscard]] HeapArray<T> make_array_in_heap(
	DomainHeap *heap, std::size_t count)
{
	if (count > static_cast<std::size_t>(-1) / sizeof(T))
		throw std::bad_alloc{};
	auto *block = static_cast<T *>(
		frame_allocate(heap, count * sizeof(T), alignof(T)));
	for (std::size_t index = 0; index < count; ++index)
		new (block + index) T{};
	return HeapArray<T>{block, HeapArrayDelete<T>{heap, count}};
}

} // namespace uml::memory

#endif
