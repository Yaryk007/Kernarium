// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* Replaces the global allocation functions of the test executable that
 * includes it (once) so that every allocation reports to the memory test
 * variant's probe. The probe counts the ones made while the frame accounting
 * holds a frame's stripe lock, where nothing may allocate: an allocation
 * through any allocator, not only through a checkpoint, is seen. A sanitizer
 * replaces the allocator itself, so the functions stay the sanitizer's. */
#include "allocation_fault.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

#if UML_MEMORY_ALLOCATION_TESTING && !__has_feature(address_sanitizer) && \
	!__has_feature(thread_sanitizer)
namespace {

[[nodiscard]] void *probed_allocate(std::size_t size, std::size_t alignment)
{
	if (::uml::memory::testing::allocator_probe())
		return nullptr;
	void *block = nullptr;
	if (alignment <= alignof(std::max_align_t))
		block = std::malloc(size == 0 ? 1 : size);
	else if (posix_memalign(&block, alignment, size == 0 ? 1 : size) != 0)
		block = nullptr;
	return block;
}

} // namespace

void *operator new(std::size_t size)
{
	if (void *block = probed_allocate(size, alignof(std::max_align_t)))
		return block;
	throw std::bad_alloc{};
}
void *operator new[](std::size_t size)
{
	return ::operator new(size);
}
void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
	return probed_allocate(size, alignof(std::max_align_t));
}
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
	return probed_allocate(size, alignof(std::max_align_t));
}
void *operator new(std::size_t size, std::align_val_t alignment)
{
	if (void *block = probed_allocate(size, static_cast<std::size_t>(alignment)))
		return block;
	throw std::bad_alloc{};
}
void *operator new[](std::size_t size, std::align_val_t alignment)
{
	return ::operator new(size, alignment);
}
void operator delete(void *block) noexcept { std::free(block); }
void operator delete[](void *block) noexcept { std::free(block); }
void operator delete(void *block, std::size_t) noexcept { std::free(block); }
void operator delete[](void *block, std::size_t) noexcept { std::free(block); }
void operator delete(void *block, std::align_val_t) noexcept { std::free(block); }
void operator delete[](void *block, std::align_val_t) noexcept { std::free(block); }
void operator delete(void *block, std::size_t, std::align_val_t) noexcept
{
	std::free(block);
}
void operator delete[](void *block, std::size_t, std::align_val_t) noexcept
{
	std::free(block);
}
#endif
